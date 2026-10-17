// SPDX-License-Identifier: GPL-2.0-or-later

// Physical melee (core/vr/melee_window.h): the move table, which row a swing opens, the damage
// factor's bounds, the presenter's window (onset, commit, abort, keep-alive, charge, a hit that
// confirms a swing), the game side's one-attack-per-window latch, the blade's bite and the
// telegraph (which swings the enemies may read), plus each move's own swing sound and trail. The
// classifier tests drive the real swing classifier (core/vr/swing_detector.h) with synthetic
// strokes at 90 Hz: a real swing opens its window at the stroke's start and commits; waggles never
// open one; bringing a raised weapon back (the QA's own scripted hand paths, the blade trailing a
// slow hand) never attacks while chops do; a quick forehand gives the enemies no notice, a wind-up
// and chop gives one, before the strike.

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/body_presets.h"
#include "core/vr/melee_settings.h"
#include "core/vr/melee_window.h"

namespace {

namespace M = VR::Melee;

const M::WeaponMoves& Moves(int variation) {
    const M::WeaponMoves* m = M::FindMoves(variation);
    if (m == nullptr) {
        static const M::WeaponMoves none{-1, "none", {}};
        return none;
    }
    return *m;
}

// Powered releases share the ordinary attack budget, but do not require hand motion.
TEST(MeleeWindow, PoweredReleaseWorksAtRestAndCannotBeExtendedByMotion) {
    M::WindowTracker tracker;
    M::TrackerConfig cfg{};
    M::TrackerInput in{};
    in.swings = true;
    in.dt = 0.01f;
    tracker.Step(in, cfg);
    const auto released = tracker.OpenPowered(in, cfg);
    ASSERT_TRUE(released.opened);
    ASSERT_TRUE(released.committed);
    EXPECT_FALSE(released.empty);
    EXPECT_EQ(tracker.Current().kind, M::MoveKind::Charged);
    EXPECT_TRUE(tracker.Current().powered);
    EXPECT_EQ(tracker.ChargeState(), M::Charge::None);
    const auto serial = tracker.Current().serial;
    for (int frame = 0; frame < 15; ++frame) {
        EXPECT_FALSE(tracker.Step(in, cfg).closed);
        EXPECT_TRUE(tracker.Current().open);
    }
    in.fired = true;
    in.fired_stroke = 123;
    in.hand_speed = 5.0f;
    in.blade_rate = 10.0f;
    for (int frame = 0; frame < 10 && tracker.Current().open; ++frame) {
        tracker.Step(in, cfg);
        EXPECT_EQ(tracker.Current().serial, serial);
    }
    EXPECT_FALSE(tracker.Current().open);
    // Once the discharge ends, a new ordinary swing is allowed again.
    EXPECT_TRUE(tracker.Step(in, cfg).opened);
    EXPECT_FALSE(tracker.Current().powered);
}

TEST(MeleeWindow, PoweredReleasePaysBudgetAndCannotUseATransformFlick) {
    M::WindowTracker tracker;
    M::TrackerConfig cfg{};
    cfg.budget.enabled = true;
    cfg.budget.charges = 2;
    M::TrackerInput in{};
    in.swings = true;
    tracker.Step(in, cfg);
    tracker.NoteFlick();
    const auto released = tracker.OpenPowered(in, cfg);
    ASSERT_TRUE(released.opened);
    EXPECT_FALSE(tracker.Current().flick);
    EXPECT_GT(tracker.Current().charges, 0);
    EXPECT_EQ(tracker.BudgetFree(), 0);
    EXPECT_TRUE(tracker.OpenPowered(in, cfg).empty);
    EXPECT_TRUE(tracker.Current().unpaid);
    tracker.Reset("context changed");
    EXPECT_EQ(tracker.ChargeState(), M::Charge::None);
}

TEST(MeleeWindow, PoweredReleaseRequiresCombatAndChargedRowAndCancelsOnBlock) {
    M::WindowTracker tracker;
    M::TrackerConfig cfg{};
    M::TrackerInput in{};
    EXPECT_FALSE(tracker.OpenPowered(in, cfg).opened);
    in.swings = true;
    in.top = M::MoveKind::Heavy;
    EXPECT_FALSE(tracker.OpenPowered(in, cfg).opened);
    in.top = M::MoveKind::Charged;
    ASSERT_TRUE(tracker.OpenPowered(in, cfg).opened);
    in.swings = false;
    EXPECT_TRUE(tracker.Step(in, cfg).closed);
    EXPECT_FALSE(tracker.Current().open);
}

// ---- The table ----------------------------------------------------------------------------------

TEST(MeleeMoves, EveryRowIsAPartOfTheWeaponInItsForm) {
    for (const M::WeaponMoves& w : M::Moves) {
        for (std::size_t i = 0; i < w.rows.size(); ++i) {
            const M::MoveRow& r = w.rows[i];
            if (!r.Valid()) {
                continue;
            }
            const bool form_b = i >= 3;
            EXPECT_TRUE(form_b ? (r.Group() >= 11 && r.Group() <= 13) : r.Group() == 10)
                << w.name << " row " << i << " dmy " << r.dmy1;
            EXPECT_GE(r.mv, 50) << w.name;
            EXPECT_GE(r.radius, 0.05f) << w.name;
            EXPECT_GE(r.stamina, 0) << w.name;
            EXPECT_GE(r.hitstop, 0.0f) << w.name;
        }
        // Every weapon in the table has a light form-A move (else it would not be listed).
        EXPECT_TRUE(w.rows[0].Valid()) << w.name;
    }
}

TEST(MeleeMoves, SawCleaverRows) {
    const M::WeaponMoves& m = Moves(700);
    ASSERT_EQ(m.variation, 700);
    EXPECT_EQ(m.rows[0].judge, 0);
    EXPECT_EQ(m.rows[0].stamina, 20);
    EXPECT_EQ(m.rows[0].mv, 100);
    EXPECT_EQ(m.rows[0].dmy1, 10100);
    EXPECT_EQ(m.rows[0].dmy2, 10110);
    EXPECT_NEAR(m.rows[0].radius, 0.4f, 1e-4f);
    EXPECT_EQ(m.rows[1].judge, 150);
    EXPECT_EQ(m.rows[1].stamina, 35);
    EXPECT_EQ(m.rows[2].judge, 170);
    EXPECT_TRUE(m.rows[2].charge);
    EXPECT_EQ(m.rows[3].judge, 400);
    EXPECT_EQ(m.rows[3].dmy1, 12100);
    EXPECT_EQ(m.rows[4].judge, 550);
    EXPECT_EQ(m.rows[5].judge, 570);
}

TEST(MeleeMoves, ChooseMoveByFormAndKind) {
    const M::WeaponMoves& saw = Moves(700);
    EXPECT_EQ(M::ChooseMove(saw, false, M::MoveKind::Light).row->judge, 0);
    EXPECT_EQ(M::ChooseMove(saw, false, M::MoveKind::Heavy).row->judge, 150);
    EXPECT_EQ(M::ChooseMove(saw, false, M::MoveKind::Charged).row->judge, 170);
    EXPECT_EQ(M::ChooseMove(saw, true, M::MoveKind::Light).row->judge, 400);
    EXPECT_EQ(M::ChooseMove(saw, true, M::MoveKind::Heavy).row->judge, 550);
    EXPECT_EQ(M::ChooseMove(saw, true, M::MoveKind::Charged).row->judge, 570);
    EXPECT_FALSE(M::ChooseMove(saw, true, M::MoveKind::Charged).lighter);
    // The Hunter Axe's form B heavy and charged are 910 / 930.
    const M::WeaponMoves& axe = Moves(500);
    EXPECT_EQ(M::ChooseMove(axe, true, M::MoveKind::Heavy).row->judge, 910);
    EXPECT_EQ(M::ChooseMove(axe, true, M::MoveKind::Charged).row->judge, 930);
    // The Threaded Cane's whip (form B) has no physical moves: R1 / R2 there.
    const M::WeaponMoves& cane = Moves(2200);
    EXPECT_TRUE(M::ChooseMove(cane, false, M::MoveKind::Charged).row != nullptr);
    EXPECT_TRUE(M::ChooseMove(cane, true, M::MoveKind::Light).row == nullptr);
    // Simon's Bowblade's charged row is its bow shot: a charged swing is its heavy.
    const M::MoveChoice simon = M::ChooseMove(Moves(3200), false, M::MoveKind::Charged);
    ASSERT_TRUE(simon.row != nullptr);
    EXPECT_EQ(simon.row->judge, 150);
    EXPECT_TRUE(simon.lighter);
    EXPECT_TRUE(M::FindMoves(1400) == nullptr); // the Hunter Pistol is no melee weapon
}

TEST(MeleeMoves, OnlyTheRightHandsParts) {
    // The swing is the right controller's: a row hitting with the blade in the LEFT hand (the
    // generator reads EquipParamWeapon's part slots) is no physical move. Rakuyo's form-B R2 is its
    // left blade (13100): a heavy swing in form B is its light row (the right blade, 12100).
    const M::WeaponMoves& rakuyo = Moves(2700);
    const M::MoveChoice rk = M::ChooseMove(rakuyo, true, M::MoveKind::Heavy);
    ASSERT_NE(rk.row, nullptr);
    EXPECT_EQ(rk.row->judge, 400);
    EXPECT_EQ(rk.row->Group(), 12);
    EXPECT_TRUE(rk.lighter);
    // The Beast Claw's form-B R2 910 / 930 swing the left claw (12100): its right claw's R2 500.
    const M::WeaponMoves& claw = Moves(900);
    const M::MoveChoice ch = M::ChooseMove(claw, true, M::MoveKind::Heavy);
    ASSERT_NE(ch.row, nullptr);
    EXPECT_EQ(ch.row->judge, 500);
    EXPECT_EQ(ch.row->Group(), 11);
    EXPECT_EQ(M::ChooseMove(claw, true, M::MoveKind::Charged).row->judge, 500);
    // The Kos Parasite's form B: every R1 hit is the left one's: no physical swing (R1 / R2).
    EXPECT_EQ(M::ChooseMove(Moves(101), true, M::MoveKind::Light).row, nullptr);
    // The Beast Cutter's form B is a whip (as the Threaded Cane's): the game's own R1 / R2.
    EXPECT_EQ(M::ChooseMove(Moves(2400), true, M::MoveKind::Light).row, nullptr);
    EXPECT_NE(M::ChooseMove(Moves(2400), false, M::MoveKind::Light).row, nullptr);
    // Weapon 390's first form-B R1 hit is the left one's: its second (410) on the right.
    EXPECT_EQ(M::ChooseMove(Moves(3700), true, M::MoveKind::Light).row->judge, 410);
    // The Kirkhammer's A->B trick attack hits with the hammer as it comes off his back (part 1,
    // not shown in form B): no transform attack, the hammer's own swing.
    EXPECT_EQ(M::FindTransformMove(800, true), nullptr);
    // Unchanged: the Saw Cleaver, the Hunter Axe, the Blade of Mercy (both its form-B rows are
    // its right blade's).
    EXPECT_EQ(M::ChooseMove(Moves(400), true, M::MoveKind::Heavy).row->judge, 500);
    EXPECT_EQ(M::ChooseMove(Moves(400), true, M::MoveKind::Heavy).row->Group(), 12);
}

TEST(MeleeMoves, TheBladesOwnSize) {
    // blade / span from the weapon models (blade_fit.py): every row knows them but the Kos
    // Parasite's (no model in the extracted files); a sphere row has no span.
    for (const M::WeaponMoves& w : M::Moves) {
        for (const M::MoveRow& r : w.rows) {
            if (!r.Valid()) {
                continue;
            }
            EXPECT_GE(r.blade, 0.0f) << w.name;
            EXPECT_LE(r.blade, 1.0f) << w.name;
            EXPECT_EQ(r.span == 0.0f, r.dmy2 < 0 || r.blade == 0.0f) << w.name << " " << r.judge;
            if (w.variation != 101) {
                EXPECT_GT(r.blade, 0.0f) << w.name << " " << r.judge;
            }
        }
    }
    // The Saw Cleaver's folded blade is wide (28 cm off the capsule's axis, its 0.28 m capsule);
    // a sword's 2 cm (the Kirkhammer's, Ludwig's and the Threaded Cane's form A).
    EXPECT_NEAR(Moves(700).rows[0].blade, 0.276f, 0.005f);
    EXPECT_NEAR(Moves(700).rows[0].span, 0.497f, 0.005f);
    EXPECT_LT(Moves(800).rows[0].blade, 0.03f);
    EXPECT_LT(Moves(810).rows[0].blade, 0.03f);
    EXPECT_LT(Moves(2200).rows[0].blade, 0.03f);
    // The Saw Cleaver's form B: the model's folded span (17 cm), the hand's unfolded one 1.1 m.
    EXPECT_LT(Moves(700).rows[3].span, 0.20f);
}

TEST(MeleeMoves, SwingRadius) {
    const M::RadiusConfig c{};
    // The Saw Cleaver: the scale's 0.28 m (its blade 28 cm off the axis: the fit changes nothing).
    const M::MoveRow& saw = Moves(700).rows[0];
    EXPECT_NEAR(M::SwingRadius(saw.radius, saw.blade, saw.span, saw.span, c), 0.28f, 1e-4f);
    // Its form B unfolds the blade (the live span 1.10 m, the model's 0.17): no fit, 0.28 m.
    const M::MoveRow& saw_b = Moves(700).rows[3];
    EXPECT_NEAR(M::SwingRadius(saw_b.radius, saw_b.blade, saw_b.span, 1.10f, c), 0.28f, 1e-4f);
    // A sword (the Kirkhammer's form A, 2 cm): 0.12 m instead of 0.28.
    const M::MoveRow& kirk = Moves(800).rows[0];
    EXPECT_NEAR(M::SwingRadius(kirk.radius, kirk.blade, kirk.span, kirk.span + 0.01f, c), 0.12f,
                0.005f);
    // ... not while the live span differs, nor with the fit off, nor without the live dummies.
    EXPECT_NEAR(M::SwingRadius(kirk.radius, kirk.blade, kirk.span, kirk.span + 0.10f, c), 0.28f,
                1e-4f);
    EXPECT_NEAR(M::SwingRadius(kirk.radius, kirk.blade, kirk.span, -1.0f, c), 0.28f, 1e-4f);
    M::RadiusConfig off = c;
    off.fit = false;
    EXPECT_NEAR(M::SwingRadius(kirk.radius, kirk.blade, kirk.span, kirk.span, off), 0.28f, 1e-4f);
    // Ludwig's greatsword (10 cm): 0.20 m instead of 0.35.
    const M::MoveRow& ludwig_b = Moves(810).rows[3];
    EXPECT_NEAR(M::SwingRadius(ludwig_b.radius, ludwig_b.blade, ludwig_b.span, ludwig_b.span, c),
                ludwig_b.blade + 0.10f, 1e-3f);
    // A thin capsule stays the game's: the Kirkhammer's hammer 0.20 m (the scale alone: 0.14), the
    // Beast Cutter's trick attack into the whip 0.15 m (0.105).
    const M::MoveRow& hammer = Moves(800).rows[3];
    EXPECT_NEAR(M::SwingRadius(hammer.radius, hammer.blade, hammer.span, hammer.span, c), 0.20f,
                1e-4f);
    const M::MoveRow* whip = M::FindTransformMove(2400, true);
    ASSERT_NE(whip, nullptr);
    EXPECT_NEAR(M::SwingRadius(whip->radius, whip->blade, whip->span, whip->span, c), 0.15f, 1e-4f);
    // The Hunter Axe's head (a sphere: no span to check the model's shape by, never fitted).
    const M::MoveRow& axe = Moves(500).rows[0];
    EXPECT_NEAR(M::SwingRadius(axe.radius, axe.blade, axe.span, 0.0f, c), 0.35f, 1e-4f);
    EXPECT_NEAR(M::SwingRadius(0.40f, 0.02f, 0.0f, 0.0f, c), 0.28f, 1e-4f);
    // The margin and the minimum; never above the row's own radius; scale 1 = the game's own.
    M::RadiusConfig wide = c;
    wide.margin = 0.30f;
    EXPECT_NEAR(M::SwingRadius(0.40f, 0.02f, 0.5f, 0.5f, wide), 0.28f, 1e-4f);
    M::RadiusConfig tight = c;
    tight.margin = 0.0f;
    EXPECT_NEAR(M::SwingRadius(0.40f, 0.02f, 0.5f, 0.5f, tight), 0.12f, 1e-4f);
    M::RadiusConfig game = off;
    game.scale = 1.0f;
    EXPECT_NEAR(M::SwingRadius(0.40f, 0.02f, 0.5f, 0.5f, game), 0.40f, 1e-4f);
    EXPECT_NEAR(M::SwingRadius(0.10f, 0.0f, 0.0f, 0.0f, c), 0.10f, 1e-4f);
}

// ---- Damage -------------------------------------------------------------------------------------

TEST(MeleeDamage, SpeedAndCadenceBounds) {
    const M::DamageConfig c{};
    // A gentle tap that committed: three quarters; full speed: +10%.
    EXPECT_NEAR(M::DamageFactor(3.0f, 5.0f, M::MoveKind::Light, c), 0.75f, 1e-4f);
    EXPECT_NEAR(M::DamageFactor(12.0f, 5.0f, M::MoveKind::Light, c), 1.10f, 1e-4f);
    const float mid = M::DamageFactor(6.0f, 5.0f, M::MoveKind::Light, c);
    EXPECT_GT(mid, 0.75f);
    EXPECT_LT(mid, 1.10f);
    // Faster than the game's own recovery: proportional, never below 0.5.
    EXPECT_NEAR(M::CadenceFactor(0.285f, 0.57f), 0.5f, 1e-4f);
    EXPECT_NEAR(M::CadenceFactor(0.1f, 0.57f), 0.5f, 1e-4f);
    EXPECT_NEAR(M::CadenceFactor(0.57f, 0.57f), 1.0f, 1e-4f);
    // Off: the row's own damage.
    M::DamageConfig off{};
    off.speed = false;
    EXPECT_EQ(M::DamageFactor(20.0f, 0.1f, M::MoveKind::Light, off), 1.0f);
}

TEST(MeleeDamage, FlailingNeverBeatsTheGamesOwnDamagePerSecond) {
    const M::DamageConfig c{};
    // The game's own light attack: one hit of 1.0 every 0.57 s. Swings at the swing gate's fastest
    // (0.35 s) and anything slower, at full blade speed: at most +10% (the speed bonus).
    const float vanilla = 1.0f / M::RecoverSeconds(M::MoveKind::Light);
    for (float interval = 0.35f; interval <= 2.0f; interval += 0.01f) {
        const float dps = M::DamageFactor(15.0f, interval, M::MoveKind::Light, c) / interval;
        EXPECT_LE(dps, vanilla * 1.10f + 1e-3f) << interval;
    }
}

// ---- The presenter's window ---------------------------------------------------------------------

struct Rig {
    M::WindowTracker tracker{};
    M::TrackerConfig cfg{};
    M::TrackerInput in{};
    M::TrackerOutput out{};
    Rig() {
        in.dt = 1.0f / 90.0f;
        in.swings = true;
        in.fwd_flat = {0.0f, 0.0f, -1.0f};
    }
    void Fast(unsigned stroke, bool cocked = false) {
        in.stroke_live = true;
        in.stroke = stroke;
        in.cocked = cocked;
        in.blade_rate = 8.0f;
        in.stroke_peak = std::max(in.stroke_peak, 8.0f);
        in.hand_speed = 3.0f;
        in.hand_v = {-3.0f, -0.3f, -0.5f}; // across the front, slightly down
        in.tip_speed = 9.0f;
    }
    void Slow() {
        in.stroke_live = false;
        in.blade_rate = 0.3f;
        in.stroke_peak = 0.0f;
        in.hand_speed = 0.2f;
        in.hand_v = {0.2f, 0.0f, 0.0f};
        in.tip_speed = 0.4f;
    }
    const M::TrackerOutput& Step() {
        out = tracker.Step(in, cfg);
        in.fired = false;
        in.kind = M::Kind::None;
        in.gate = M::Kind::None;
        return out;
    }
    const M::Window& W() const {
        return tracker.Current();
    }
};

TEST(MeleeWindow, OnsetThenCommitThenCloseWhenSlow) {
    Rig r;
    r.Slow();
    r.Step();
    EXPECT_EQ(r.W().serial, 0u);
    r.Fast(1);
    EXPECT_TRUE(r.Step().opened);
    EXPECT_TRUE(r.W().open);
    EXPECT_FALSE(r.W().committed);
    EXPECT_EQ(r.W().kind, M::MoveKind::Light);
    for (int i = 0; i < 6; ++i) {
        r.Step();
    }
    r.in.fired = true;
    r.in.fired_stroke = 1;
    r.in.kind = M::Kind::Light;
    r.in.kind_stroke = 1;
    EXPECT_TRUE(r.Step().committed);
    EXPECT_TRUE(r.W().committed);
    EXPECT_FALSE(r.out.opened); // the same window
    for (int i = 0; i < 20; ++i) {
        r.Step();
        EXPECT_TRUE(r.W().open);
    }
    r.Slow();
    r.Step();
    r.Step();
    EXPECT_TRUE(r.W().open); // 30 ms below the keep rates first
    for (int i = 0; i < 3; ++i) {
        r.Step();
    }
    EXPECT_FALSE(r.W().open);
    EXPECT_FALSE(r.W().aborted);
    EXPECT_EQ(r.W().serial, 1u);
}

TEST(MeleeWindow, OnsetWithoutCommitIsAborted) {
    Rig r;
    r.Fast(1);
    r.Step();
    ASSERT_TRUE(r.W().open);
    int frames = 0;
    while (r.W().open && frames < 60) {
        r.Step();
        ++frames;
    }
    EXPECT_FALSE(r.W().open);
    EXPECT_TRUE(r.W().aborted);
    EXPECT_NEAR(frames / 90.0f, 0.15f, 0.02f);
    // A wind-up the gate ignores: aborted at once.
    Rig g;
    g.Fast(1);
    g.Step();
    g.in.gate = M::Kind::Ignored;
    EXPECT_TRUE(g.Step().aborted);
}

TEST(MeleeWindow, AWhipSweepIsNeverAbortedOnceItsTipWasFast) {
    // The whip forms (commit_tip = whip_payout_speed): a sweep the swing detector never fires on
    // commits at confirm_s instead of being aborted, once its far end got fast, and lives on.
    Rig r;
    r.cfg.commit_tip = 5.5f;
    r.Fast(1);
    r.in.tip_speed = 4.0f;
    r.Step();
    ASSERT_TRUE(r.W().open);
    r.in.tip_speed = 9.0f;
    r.Step();
    EXPECT_FALSE(r.W().committed); // the gate has until confirm_s
    bool committed = false;
    for (int i = 0; i < 30; ++i) {
        committed = r.Step().committed || committed;
    }
    EXPECT_TRUE(committed);
    EXPECT_TRUE(r.W().open);
    EXPECT_FALSE(r.W().aborted);
    // The lash whose arm stroke ended early (the hand stops, the chain flies on): committed too.
    Rig e;
    e.cfg.commit_tip = 5.5f;
    e.Fast(1);
    e.Step();
    e.in.stroke_live = false;
    EXPECT_TRUE(e.Step().committed);
    EXPECT_FALSE(e.W().aborted);
    // A fast raise or wind-up the gate ignores, even after the tip was fast: aborted.
    Rig v;
    v.cfg.commit_tip = 5.5f;
    v.Fast(1);
    v.Step();
    for (int i = 0; i < 4; ++i) {
        v.Step();
    }
    v.in.gate = M::Kind::Ignored;
    EXPECT_TRUE(v.Step().aborted);
    // Too slow a sweep: aborted as before; a raise the gate ignores at once: aborted.
    Rig s;
    s.cfg.commit_tip = 5.5f;
    s.Fast(1);
    s.in.tip_speed = 4.0f;
    s.Step();
    int frames = 0;
    while (s.W().open && frames < 60) {
        s.Step();
        ++frames;
    }
    EXPECT_TRUE(s.W().aborted);
    Rig g;
    g.cfg.commit_tip = 5.5f;
    g.Fast(1);
    g.Step();
    g.in.gate = M::Kind::Ignored;
    EXPECT_TRUE(g.Step().aborted);
}

TEST(MeleeWindow, AHitConfirmsAnOnsetWindowAndMergesTheNextStroke) {
    Rig r;
    r.Fast(1);
    r.Step();
    r.in.hit_serial = 1; // the game: window 1 hit the beast
    EXPECT_TRUE(r.Step().committed);
    r.in.gate = M::Kind::Ignored; // the gate would have called it a wind-up
    r.Step();
    EXPECT_TRUE(r.W().open);
    // The classifier splits the motion: a new stroke commits 0.2 s later: still window 1.
    r.Fast(2);
    for (int i = 0; i < 15; ++i) {
        r.Step();
    }
    r.in.fired = true;
    r.in.fired_stroke = 2;
    EXPECT_FALSE(r.Step().opened);
    EXPECT_EQ(r.W().serial, 1u);
    EXPECT_TRUE(r.W().open);
}

TEST(MeleeWindow, CommitModeOpensAtTheCommit) {
    Rig r;
    r.cfg.onset = false;
    r.Fast(1);
    for (int i = 0; i < 8; ++i) {
        r.Step();
        EXPECT_FALSE(r.W().open);
    }
    r.in.fired = true;
    r.in.fired_stroke = 1;
    const auto& out = r.Step();
    EXPECT_TRUE(out.opened);
    EXPECT_TRUE(out.committed);
    EXPECT_TRUE(r.W().committed);
}

TEST(MeleeWindow, PullBackAndRaiseNeverOpenAtTheOnset) {
    Rig r;
    r.Fast(1);
    r.in.hand_v = {0.0f, 0.2f, 3.0f}; // toward the body
    r.Step();
    EXPECT_FALSE(r.W().open);
    Rig u;
    u.Fast(1);
    u.in.hand_v = {0.3f, 2.9f, -0.2f}; // straight up
    u.Step();
    EXPECT_FALSE(u.W().open);
    // A slow hand with a fast blade (the wrist): no window.
    Rig w;
    w.Fast(1);
    w.in.hand_speed = 0.8f;
    w.in.hand_v = {-0.8f, 0.0f, 0.0f};
    w.Step();
    EXPECT_FALSE(w.W().open);
}

TEST(MeleeWindow, MinIntervalBetweenCountedSwings) {
    // melee_budget=0 (with the combo budget the budget sets the rate: MeleeBudget below).
    Rig r;
    r.cfg.budget.enabled = false;
    r.Fast(1);
    r.Step();
    r.in.fired = true;
    r.in.fired_stroke = 1;
    r.Step();
    r.Slow();
    for (int i = 0; i < 6; ++i) {
        r.Step();
    }
    ASSERT_FALSE(r.W().open);
    // A new stroke 0.1 s later: no onset window before 0.35 s after the last one opened.
    r.Fast(2);
    r.Step();
    EXPECT_FALSE(r.W().open);
    for (int i = 0; i < 30; ++i) {
        r.in.stroke = 3 + i; // fresh strokes each frame (worst case)
        r.Step();
        if (r.W().open) {
            break;
        }
    }
    EXPECT_TRUE(r.W().open);
    EXPECT_EQ(r.W().serial, 2u);
    EXPECT_GE(r.W().since_counted, 0.35f - 1e-3f);
    // An onset window that aborted without a hit does not count: the next one may open at once.
    Rig a;
    a.Fast(1);
    a.Step();
    a.in.gate = M::Kind::Ignored;
    a.Step();
    ASSERT_FALSE(a.W().open);
    a.Fast(2);
    a.Step();
    EXPECT_TRUE(a.W().open);
    EXPECT_EQ(a.W().serial, 2u);
}

TEST(MeleeWindow, ClosesAfterMaxOpenAndWhenSwingsGoOff) {
    Rig r;
    r.Fast(1);
    r.Step();
    r.in.fired = true;
    r.in.fired_stroke = 1;
    r.Step();
    int frames = 0;
    while (r.W().open && frames < 200) {
        r.Step();
        ++frames;
    }
    EXPECT_NEAR(frames / 90.0f, 0.6f, 0.03f);
    Rig s;
    s.Fast(1);
    s.Step();
    s.in.swings = false;
    s.Step();
    EXPECT_FALSE(s.W().open);
}

TEST(MeleeWindow, WoundUpStrokeOpensHeavyAndTakesTheVerdict) {
    Rig r;
    r.Fast(4, true);
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().kind, M::MoveKind::Heavy);
    EXPECT_FALSE(r.W().verdict_known);
    r.in.fired = true;
    r.in.fired_stroke = 4;
    r.Step();
    for (int i = 0; i < 10; ++i) {
        r.Step();
    }
    r.in.kind = M::Kind::Light; // the arc never got there
    r.in.kind_stroke = 4;
    EXPECT_TRUE(r.Step().verdict);
    EXPECT_TRUE(r.W().verdict_known);
    EXPECT_EQ(r.W().verdict, M::MoveKind::Light);
    // A stroke that did not start wound up is light at once.
    Rig l;
    l.Fast(1);
    l.Step();
    EXPECT_TRUE(l.W().verdict_known);
    EXPECT_EQ(l.W().verdict, M::MoveKind::Light);
}

TEST(MeleeWindow, TriggerChargeArmsHeavyThenCharged) {
    Rig r;
    r.Slow();
    r.in.trigger = true;
    bool heavy_edge = false, full_edge = false;
    for (int i = 0; i < 40; ++i) { // 0.44 s
        const auto& out = r.Step();
        heavy_edge = heavy_edge || out.charge_heavy;
    }
    EXPECT_TRUE(heavy_edge);
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::Heavy);
    for (int i = 0; i < 60; ++i) { // past 1.0 s
        full_edge = full_edge || r.Step().charge_full;
    }
    EXPECT_TRUE(full_edge);
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::Full);
    // Let go: still armed for 1.5 s; the next swing is charged and uses it up.
    r.in.trigger = false;
    for (int i = 0; i < 90; ++i) {
        r.Step();
    }
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::Full);
    r.Fast(1);
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().kind, M::MoveKind::Charged);
    EXPECT_TRUE(r.W().verdict_known);
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::None);
    // Not used: gone 1.5 s after the release.
    Rig g;
    g.Slow();
    g.in.trigger = true;
    for (int i = 0; i < 40; ++i) {
        g.Step();
    }
    g.in.trigger = false;
    for (int i = 0; i < 140; ++i) {
        g.Step();
    }
    EXPECT_EQ(g.tracker.ChargeState(), M::Charge::None);
}

TEST(MeleeWindow, AWindUpTheGateRefusesKeepsTheCharge) {
    Rig r;
    r.Slow();
    r.in.trigger = true;
    for (int i = 0; i < 40; ++i) { // heavy armed
        r.Step();
    }
    r.in.trigger = false;
    r.Step();
    ASSERT_EQ(r.tracker.ChargeState(), M::Charge::Heavy);
    r.Fast(1); // the wind-up opens a heavy onset window ...
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().kind, M::MoveKind::Heavy);
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::None);
    r.in.gate = M::Kind::Ignored; // ... the gate calls it a wind-up
    EXPECT_TRUE(r.Step().aborted);
    EXPECT_EQ(r.tracker.ChargeState(), M::Charge::Heavy); // still armed for the strike
    r.Fast(2);
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().kind, M::MoveKind::Heavy);
}

// ---- The game's latch ---------------------------------------------------------------------------

// One step, checked (the step has side effects: evaluated once).
void Expect(M::LatchState& s, const M::LatchInput& in, M::LatchStep want, int line) {
    const M::LatchStep got = M::StepLatch(s, in);
    EXPECT_EQ(got, want) << "line " << line << ": " << M::LatchStepName(got) << " instead of "
                         << M::LatchStepName(want);
}

TEST(MeleeLatch, OneAttackPerWindow) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = true;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    Expect(s, in, M::LatchStep::Refresh, __LINE__);
    Expect(s, in, M::LatchStep::Refresh, __LINE__);
    in.open = false;
    Expect(s, in, M::LatchStep::End, __LINE__);
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    // A new window: a new attack (the other slot).
    in.serial = 2;
    in.open = true;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    // A new window while one is alive: the old one is not called again, the new one created.
    in.serial = 3;
    Expect(s, in, M::LatchStep::Create, __LINE__);
}

TEST(MeleeLatch, AWindowClosedBeforeTheFirstFrameStillGetsTwoFrames) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = false; // opened and closed between two game frames
    in.gates = true;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    Expect(s, in, M::LatchStep::Refresh, __LINE__);
    Expect(s, in, M::LatchStep::End, __LINE__);
    // Stale (closed long ago, e.g. before a load): nothing.
    M::LatchState t{};
    in.serial = 2;
    in.stale = true;
    Expect(t, in, M::LatchStep::Idle, __LINE__);
    Expect(t, in, M::LatchStep::Idle, __LINE__);
}

TEST(MeleeLatch, UnpaidWindowCannotUseMinimumFramesToCreateAnAttack) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = false;
    in.gates = true;
    in.paid = false;
    for (int i = 0; i < 8; ++i) {
        Expect(s, in, M::LatchStep::Wait, __LINE__);
        EXPECT_FALSE(s.active);
    }
    // The presenter may pay at commit, even after the game first sees the window.
    in.paid = true;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    Expect(s, in, M::LatchStep::Refresh, __LINE__);
    Expect(s, in, M::LatchStep::End, __LINE__);
}

TEST(MeleeLatch, UnpaidWindowStillHonorsGatesAndCannotRefreshAnAttack) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = false;
    in.paid = false;
    Expect(s, in, M::LatchStep::Deny, __LINE__);
    in.gates = true;
    in.paid = true;
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    in.serial = 2;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    in.paid = false;
    Expect(s, in, M::LatchStep::End, __LINE__);
}

TEST(MeleeLatch, PaymentCannotReviveAStaleWaitingWindow) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = true;
    in.paid = false;
    Expect(s, in, M::LatchStep::Wait, __LINE__);
    in.open = false;
    in.stale = true;
    in.paid = true;
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    EXPECT_FALSE(s.active);
    EXPECT_TRUE(s.done);
}

TEST(MeleeLatch, PaymentGuardPreservesExistingDelayedWhipRelease) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = true;
    in.ready = false; // waiting for the chain's tip to reach speed, already paid
    in.max_wait = 100;
    Expect(s, in, M::LatchStep::Wait, __LINE__);
    in.open = false;
    in.stale = true;
    in.ready = true; // the lash can follow a closed controller window
    Expect(s, in, M::LatchStep::Create, __LINE__);
}

TEST(MeleeLatch, AGapEndsItForGoodAndADeniedWindowStaysDenied) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = true;
    Expect(s, in, M::LatchStep::Create, __LINE__);
    in.gates = false; // a hit taken mid-swing
    Expect(s, in, M::LatchStep::End, __LINE__);
    in.gates = true; // the arm back while the window is still open: never a second attack
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    M::LatchState d{};
    in.serial = 2;
    in.gates = false;
    Expect(d, in, M::LatchStep::Deny, __LINE__);
    in.gates = true;
    Expect(d, in, M::LatchStep::Idle, __LINE__);
}

TEST(MeleeLatch, WaitsForTheVisceralCheckAtMostTwoFrames) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 1;
    in.open = true;
    in.gates = true;
    in.ready = false;
    Expect(s, in, M::LatchStep::Wait, __LINE__);
    Expect(s, in, M::LatchStep::Wait, __LINE__);
    Expect(s, in, M::LatchStep::Create, __LINE__);
    M::LatchState r{};
    in.serial = 2;
    Expect(r, in, M::LatchStep::Wait, __LINE__);
    in.ready = true;
    Expect(r, in, M::LatchStep::Create, __LINE__);
    // The visceral started: the gates fail (a throw) and the window is refused.
    M::LatchState v{};
    in.serial = 3;
    in.gates = false;
    Expect(v, in, M::LatchStep::Deny, __LINE__);
}

// The swing buffer (swing_buffer_ms=150), as MeleePoseTick composes it with the latch: a new
// swing refused only by "the game takes no attack yet" waits for the game's window (its attack
// opens the tick the window opens), at most 150 ms; any other refusal, a closed window or 0 ms:
// refused at once (the deny tick); a swing already attacking is never held.
enum class Gate { Ok, Wait, Other };

M::LatchStep BufferTick(M::LatchState& s, M::SwingBuffer& b, std::uint32_t serial, bool open,
                        Gate gate, std::int64_t now, std::int64_t buffer_ns) {
    M::LatchInput in{};
    in.have = true;
    in.serial = serial;
    in.open = open;
    in.gates = gate == Gate::Ok;
    const bool pending = serial != s.serial || (!s.active && !s.done);
    if (b.Hold(serial, pending, gate == Gate::Wait, open, now, buffer_ns)) {
        in.ready = false;
        in.max_wait = 1u << 30;
    }
    return M::StepLatch(s, in);
}

TEST(MeleeLatch, TheSwingBufferWaitsForTheGamesWindow) {
    constexpr std::int64_t Frame = 16'666'667; // 60 Hz ticks (rounded up: tick 9 is 150 ms)
    constexpr std::int64_t Buffer = 150'000'000;
    M::SwingBuffer b{};
    // One tick, checked (the step has side effects: evaluated once).
    const auto expect = [&b](M::LatchState& s, std::uint32_t serial, bool open, Gate gate,
                             std::int64_t now, std::int64_t buffer_ns, M::LatchStep want,
                             int line) {
        const M::LatchStep got = BufferTick(s, b, serial, open, gate, now, buffer_ns);
        EXPECT_EQ(got, want) << "line " << line << ": " << M::LatchStepName(got)
                             << " instead of " << M::LatchStepName(want);
    };
    // The stagger's window opens 6 ticks (100 ms) after the swing: held 6 ticks, created on the
    // 7th (the tick it opens).
    M::LatchState s{};
    std::int64_t now = 0;
    int waits = 0;
    M::LatchStep step{};
    for (int i = 0; i < 20; ++i, now += Frame) {
        step = BufferTick(s, b, 1, true, i < 6 ? Gate::Wait : Gate::Ok, now, Buffer);
        if (step != M::LatchStep::Wait) {
            break;
        }
        ++waits;
    }
    EXPECT_EQ(step, M::LatchStep::Create);
    EXPECT_EQ(waits, 6);
    const std::int64_t held = b.HeldNs(1, now);
    EXPECT_EQ(held, 6 * Frame);
    // ... and it lives on as a normal attack (a later gap still ends it).
    expect(s, 1, true, Gate::Ok, now += Frame, Buffer, M::LatchStep::Refresh, __LINE__);
    expect(s, 1, true, Gate::Wait, now += Frame, Buffer, M::LatchStep::End, __LINE__);
    expect(s, 1, true, Gate::Ok, now += Frame, Buffer, M::LatchStep::Idle, __LINE__);

    // The window opens too late (18 ticks, 300 ms): held to 150 ms, then refused (deny tick).
    M::LatchState l{};
    now = 0;
    waits = 0;
    for (int i = 0; i < 30; ++i, now += Frame) {
        step = BufferTick(l, b, 2, true, i < 18 ? Gate::Wait : Gate::Ok, now, Buffer);
        if (step != M::LatchStep::Wait) {
            break;
        }
        ++waits;
    }
    EXPECT_EQ(step, M::LatchStep::Deny);
    EXPECT_EQ(waits, 9); // ticks at 0 .. 133 ms held; refused at 150 ms
    expect(l, 2, true, Gate::Ok, now += Frame, Buffer, M::LatchStep::Idle, __LINE__);

    // Any other refusal (a menu, a grab, a cinematic, death): at once, also mid-hold.
    M::LatchState o{};
    expect(o, 3, true, Gate::Other, 0, Buffer, M::LatchStep::Deny, __LINE__);
    M::LatchState m{};
    expect(m, 4, true, Gate::Wait, 0, Buffer, M::LatchStep::Wait, __LINE__);
    expect(m, 4, true, Gate::Other, Frame, Buffer, M::LatchStep::Deny, __LINE__);
    // The swing's window closed while held (the motion is over): refused.
    M::LatchState c{};
    expect(c, 5, true, Gate::Wait, 0, Buffer, M::LatchStep::Wait, __LINE__);
    expect(c, 5, false, Gate::Wait, Frame, Buffer, M::LatchStep::Deny, __LINE__);
    // swing_buffer_ms=0: refused at once (as before).
    M::LatchState z{};
    expect(z, 6, true, Gate::Wait, 0, 0, M::LatchStep::Deny, __LINE__);
    const std::int64_t never = b.HeldNs(6, Frame);
    EXPECT_EQ(never, 0);
}

TEST(MeleeBite, HoldThenEaseBack) {
    EXPECT_EQ(M::BiteFollow(-0.01f, 0.07f, 0.08f, 0.5f), 1.0f);
    EXPECT_EQ(M::BiteFollow(0.0f, 0.07f, 0.08f, 0.5f), 0.5f);
    EXPECT_EQ(M::BiteFollow(0.06f, 0.07f, 0.08f, 0.5f), 0.5f);
    const float mid = M::BiteFollow(0.11f, 0.07f, 0.08f, 0.5f);
    EXPECT_GT(mid, 0.5f);
    EXPECT_LT(mid, 1.0f);
    EXPECT_EQ(M::BiteFollow(0.15f, 0.07f, 0.08f, 0.5f), 1.0f);
}

// ---- With the real classifier: synthetic strokes at 90 Hz --------------------------------------

constexpr float Pi = 3.14159265358979f;

M::Q4 AxisAngle(M::V3 axis, float angle) {
    const M::V3 a = M::Normalize(axis);
    const float s = std::sin(0.5f * angle);
    return {a.x * s, a.y * s, a.z * s, std::cos(0.5f * angle)};
}

M::Q4 Mul(const M::Q4& a, const M::Q4& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

float MinJerk(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    return s * s * s * (10.0f - 15.0f * s + 6.0f * s * s);
}

struct Sample {
    M::V3 p{};
    M::V3 v{};
    M::Q4 q{};
    bool tracked{true}; // position tracked (the mock's `lost` preset clears it)
};

// The blade (grip -Z) pointing out from the right shoulder through the hand, swept by `arc`
// radians about `axis` from `start` over `seconds` (minimum jerk); `still` seconds before and
// after.
std::vector<Sample> Stroke(M::V3 start, M::V3 axis, float arc, float seconds, float still,
                           float reach = 0.55f) {
    const M::V3 shoulder{0.18f, 1.38f, 0.05f};
    std::vector<Sample> out;
    const float dt = 1.0f / 90.0f;
    const int n = static_cast<int>((2.0f * still + seconds) / dt);
    // The grip whose -Z is `start`: the shortest turn from -Z.
    const M::V3 z{0.0f, 0.0f, -1.0f};
    const M::V3 s0 = M::Normalize(start);
    const M::V3 c = M::Cross(z, s0);
    const float ang = std::atan2(M::Length(c), M::Dot(z, s0));
    const M::Q4 q0 = M::Length(c) > 1e-6f ? AxisAngle(c, ang) : M::Q4{};
    M::V3 prev{};
    for (int i = 0; i <= n; ++i) {
        const float t = i * dt;
        const float k = MinJerk((t - still) / seconds);
        const M::Q4 q = Mul(AxisAngle(axis, arc * k), q0);
        const M::V3 b = M::Rotate(q, z);
        Sample s{};
        s.q = q;
        s.p = shoulder + b * reach;
        s.v = i > 0 ? (s.p - prev) * (1.0f / dt) : M::V3{};
        prev = s.p;
        out.push_back(s);
    }
    return out;
}

struct Sim {
    M::Classifier classifier{};
    M::WindowTracker tracker{};
    M::TrackerConfig tcfg{};
    int opened = 0, committed = 0, aborted = 0, closed = 0;
    float first_open_t = -1.0f, first_commit_t = -1.0f, onset_t = -1.0f;
    // The telegraph (melee_telegraph=heavy unless set), as the presenter steps it.
    M::TelegraphTracker telegraph{};
    M::TelegraphConfig gcfg{};
    int telegraphs = 0;
    float first_telegraph_t = -1.0f;
    const char* telegraph_why = "";
    M::MoveKind first_kind{M::MoveKind::Light};

    int recovery_strokes = 0;
    unsigned last_recovery = 0;
    // The window open in each frame.
    std::vector<bool> open_frames;
    // The presenter's SwingGate, its face part only (right_grip=transform): swings are off while
    // the hand is at the face (M::HandAtFace); back on at once for a strike out of the zone (2 m/s
    // away from the head), else once the hand has been slower than 1 m/s for 0.1 s (with the
    // classifier's OnDraw).
    bool face{false};
    M::FaceRule face_rule{};
    int face_offs = 0;
    bool overhead{true}; // false: the overhead rules off (windup_turn, toward_front, rejudge)

    bool FaceGate(const Sample& s, float dt) {
        const M::V3 head{0.0f, 1.6f, 0.0f};
        const M::V3 d = s.p - head;
        const float distance = M::Length(d);
        at_face = M::HandAtFace(at_face, -d.z, distance, M::Length(s.v), face_rule);
        if (at_face) {
            face_offs += ready ? 1 : 0;
            ready = false;
            from_face = true;
            quiet = 0.0f;
            return false;
        }
        if (!ready) {
            if (from_face && distance > 1e-4f && M::Dot(s.v, d) / distance >= 2.0f) {
                ready = true;
                from_face = false;
                return true;
            }
            quiet = M::Length(s.v) < 1.0f ? quiet + dt : 0.0f;
            if (quiet >= 0.1f) {
                ready = true;
                from_face = false;
                classifier.OnDraw();
            }
        }
        return ready;
    }
    bool at_face{false}, ready{true}, from_face{false};
    float quiet{0.0f};

    void Run(const std::vector<Sample>& samples, float hz = 90.0f) {
        auto cfg = M::Config::Make(true, true, 140.0f, 100.0f, false, 0.0f);
        cfg.clutchless = true;
        if (!overhead) {
            cfg.windup_turn = 0.0f;
            cfg.toward_front = 0.0f;
            cfg.rejudge = false;
        }
        const float dt = 1.0f / hz;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Sample& s = samples[i];
            const bool swings = !face || FaceGate(s, dt);
            M::Input in{};
            in.t = 1.0 + i * dt;
            in.active = swings;
            in.immersive = true;
            in.heavy_idle = true;
            in.ori_valid = true;
            in.q = s.q;
            in.pos_valid = s.tracked;
            in.p = s.p;
            in.v = s.v;
            in.head_ok = true;
            in.head_p = {0.0f, 1.6f, 0.0f};
            in.head_fwd = {0.0f, 0.0f, -1.0f};
            const M::Output o = classifier.Update(in, cfg);
            if (onset_t < 0.0f && classifier.StrokeLive()) {
                onset_t = i * dt;
            }
            M::TrackerInput ti{};
            ti.dt = dt;
            ti.swings = swings;
            ti.stroke_live = classifier.StrokeLive();
            ti.stroke = classifier.CurrentStroke();
            ti.cocked = classifier.CurrentCocked();
            ti.recovery = classifier.Recovery();
            if (ti.recovery && ti.stroke != last_recovery) {
                last_recovery = ti.stroke;
                ++recovery_strokes;
            }
            ti.blade_rate = classifier.Rate();
            ti.stroke_peak = classifier.StrokePeak();
            ti.fired = o.fired;
            ti.fired_stroke = o.fired_stroke;
            ti.kind = o.kind;
            ti.kind_stroke = o.stats.id;
            ti.gate = o.gate;
            ti.hand_v = s.v;
            ti.hand_speed = M::Length(s.v);
            ti.fwd_flat = {0.0f, 0.0f, -1.0f};
            ti.tip_speed = ti.hand_speed;
            const M::TrackerOutput out = tracker.Step(ti, tcfg);
            open_frames.push_back(tracker.Current().open);
            M::TelegraphInput gi{};
            gi.dt = dt;
            gi.swings = swings;
            gi.stroke_live = classifier.StrokeLive();
            gi.stroke = classifier.CurrentStroke();
            gi.windup = classifier.CurrentWindup();
            gi.stroke_arc = classifier.CurrentArcDeg() * Pi / 180.0f;
            gi.blade_angle = classifier.BladeAngle();
            gi.wound_up = classifier.WoundUpPose(cfg);
            gi.window = &tracker.Current();
            gi.opened = out.opened;
            gcfg.wide_arc = cfg.heavy_arc;
            if (const M::TelegraphOutput g = telegraph.Step(gi, gcfg); g.fire) {
                if (telegraphs++ == 0) {
                    first_telegraph_t = i * dt;
                    telegraph_why = g.why;
                }
            }
            if (out.opened) {
                ++opened;
                if (first_open_t < 0.0f) {
                    first_open_t = i * dt;
                    first_kind = tracker.Current().kind;
                }
            }
            if (out.committed) {
                ++committed;
                if (first_commit_t < 0.0f) {
                    first_commit_t = i * dt;
                }
            }
            aborted += out.aborted ? 1 : 0;
            closed += out.closed ? 1 : 0;
        }
    }
};

TEST(MeleeWindowClassifier, AForehandOpensAtTheStrokesStartAndCommits) {
    // Out to the right, swept 120 degrees across the front in 0.35 s (hand peak ~3.5 m/s).
    const M::V3 start{0.96f, 0.0f, -0.26f};
    Sim sim;
    sim.Run(Stroke(start, {0.0f, 1.0f, 0.0f}, 120.0f * Pi / 180.0f, 0.35f, 0.4f));
    EXPECT_EQ(sim.opened, 1);
    EXPECT_EQ(sim.committed, 1);
    EXPECT_EQ(sim.aborted, 0);
    ASSERT_GE(sim.first_open_t, 0.0f);
    ASSERT_GE(sim.first_commit_t, 0.0f);
    // Open no later than the swing gate commits it (earlier: the point-blank hits the commit
    // alone loses).
    EXPECT_LE(sim.first_open_t, sim.first_commit_t);
    std::printf("forehand: stroke start %.0f ms, window %.0f ms, commit %.0f ms\n",
                sim.onset_t * 1000.0f, sim.first_open_t * 1000.0f, sim.first_commit_t * 1000.0f);
    EXPECT_FALSE(sim.tracker.Current().open);
    // And with melee_open=commit, at the commit.
    Sim late;
    late.tcfg.onset = false;
    late.Run(Stroke(start, {0.0f, 1.0f, 0.0f}, 120.0f * Pi / 180.0f, 0.35f, 0.4f));
    EXPECT_EQ(late.opened, 1);
    EXPECT_NEAR(late.first_open_t, sim.first_commit_t, 1.5f / 90.0f);
}

TEST(MeleeWindowClassifier, SlowSwingsAndWaggleNeverOpen) {
    const M::V3 start{0.96f, 0.0f, -0.26f};
    // The same arc in 1.4 s (hand peak ~0.9 m/s): no window.
    Sim slow;
    slow.Run(Stroke(start, {0.0f, 1.0f, 0.0f}, 120.0f * Pi / 180.0f, 1.4f, 0.4f));
    EXPECT_EQ(slow.opened, 0);
    // A wrist waggle: the blade +-35 deg at 4 Hz, the hand still.
    std::vector<Sample> waggle;
    const float dt = 1.0f / 90.0f;
    for (int i = 0; i < 270; ++i) {
        const float a = 35.0f * Pi / 180.0f * std::sin(2.0f * Pi * 4.0f * i * dt);
        Sample s{};
        s.q = AxisAngle({0.0f, 1.0f, 0.0f}, a);
        s.p = {0.18f, 1.25f, -0.40f};
        waggle.push_back(s);
    }
    Sim w;
    w.Run(waggle);
    EXPECT_EQ(w.opened, 0);
    // Arm waggles: +-8 and +-10 cm at 3 Hz (hand peaks 1.5 and 1.9 m/s), the blade +-20 deg.
    for (const float amp : {0.08f, 0.10f}) {
        std::vector<Sample> arm;
        for (int i = 0; i < 270; ++i) {
            const float ph = 2.0f * Pi * 3.0f * i * dt;
            Sample s{};
            s.q = AxisAngle({0.0f, 1.0f, 0.0f}, 0.35f * std::sin(ph));
            s.p = {0.18f + amp * std::sin(ph), 1.25f, -0.40f};
            s.v = {amp * 2.0f * Pi * 3.0f * std::cos(ph), 0.0f, 0.0f};
            arm.push_back(s);
        }
        Sim a;
        a.Run(arm);
        EXPECT_EQ(a.opened, 0) << amp;
    }
}

// ---- Bringing the weapon back (the QA's hand paths) -------------------------------------------

namespace BP = VR::BodyPresets;

M::Q4 ToQ4(const std::array<float, 4>& q) {
    return {q[0], q[1], q[2], q[3]};
}

// The mock runtime's scripted right grip (the test harness MOCKXR_HANDS=<presets>,
// MOCKXR_HAND_BLEND=<blend>): each 3 s segment holds its preset, its first `blend` seconds moving
// from the last segment's pose (position minimum jerk, orientation normalized lerp), velocities by
// finite differences; `loops` times through the list, at `hz`.
std::vector<Sample> PresetPath(std::initializer_list<std::string_view> names, float blend,
                               int loops, float hz) {
    std::vector<const BP::Preset*> timeline;
    for (const std::string_view name : names) {
        timeline.push_back(BP::Find(name));
    }
    std::vector<Sample> out;
    const double dt = 1.0 / hz;
    const double total = BP::SegmentSeconds * static_cast<double>(timeline.size() * loops);
    M::V3 prev{};
    for (int i = 0; i * dt < total; ++i) {
        const double t = i * dt;
        const BP::TimelinePoint point = BP::Locate(timeline, t);
        BP::HandPose pose = BP::Evaluate(*point.preset, 1, point.t);
        if (blend > 0.0f && point.t < blend) {
            const BP::Preset* previous =
                point.segment == 0
                    ? BP::Find("rest")
                    : timeline[(point.index + timeline.size() - 1) % timeline.size()];
            const BP::HandPose from = BP::Evaluate(*previous, 1, BP::SegmentSeconds);
            const float k = static_cast<float>(BP::MinJerk(point.t / blend));
            float dot = 0.0f;
            for (int j = 0; j < 4; ++j) {
                dot += from.q[j] * pose.q[j];
            }
            const float sign = dot < 0.0f ? -1.0f : 1.0f;
            float norm = 0.0f;
            for (int j = 0; j < 3; ++j) {
                pose.p[j] = from.p[j] + (pose.p[j] - from.p[j]) * k;
            }
            for (int j = 0; j < 4; ++j) {
                pose.q[j] = from.q[j] + (sign * pose.q[j] - from.q[j]) * k;
                norm += pose.q[j] * pose.q[j];
            }
            for (float& c : pose.q) {
                c /= std::sqrt(norm);
            }
        }
        Sample s{};
        s.p = {pose.p[0], pose.p[1], pose.p[2]};
        s.q = ToQ4(pose.q);
        s.tracked = pose.tracked;
        s.v = i > 0 ? (s.p - prev) * static_cast<float>(hz) : M::V3{};
        prev = s.p;
        out.push_back(s);
    }
    return out;
}

// From preset `a`'s pose to `b`'s over `seconds` (the mock's blend), still before and after.
std::vector<Sample> Move(std::string_view a, std::string_view b, float seconds, float hz) {
    const BP::HandPose from = BP::Evaluate(*BP::Find(a), 1, 0.0);
    BP::HandPose to = BP::Evaluate(*BP::Find(b), 1, 0.0);
    float dot = 0.0f;
    for (int j = 0; j < 4; ++j) {
        dot += from.q[j] * to.q[j];
    }
    std::vector<Sample> out;
    const float dt = 1.0f / hz;
    M::V3 prev{};
    for (int i = 0; i * dt < seconds + 0.8f; ++i) {
        const float k = MinJerk((i * dt - 0.4f) / seconds);
        Sample s{};
        float q[4];
        float norm = 0.0f;
        for (int j = 0; j < 4; ++j) {
            q[j] = from.q[j] + ((dot < 0.0f ? -1.0f : 1.0f) * to.q[j] - from.q[j]) * k;
            norm += q[j] * q[j];
        }
        s.q = {q[0] / std::sqrt(norm), q[1] / std::sqrt(norm), q[2] / std::sqrt(norm),
               q[3] / std::sqrt(norm)};
        s.p = {from.p[0] + (to.p[0] - from.p[0]) * k, from.p[1] + (to.p[1] - from.p[1]) * k,
               from.p[2] + (to.p[2] - from.p[2]) * k};
        s.v = i > 0 ? (s.p - prev) * hz : M::V3{};
        prev = s.p;
        out.push_back(s);
    }
    return out;
}

TEST(MeleeWindowClassifier, LoweringARaisedArmIsNoSwing) {
    // QA (runs/c5, 2026-10-04): the right hand raised high (`high`: 45 cm above the eyes, blade
    // 70 deg up) and lowered across the body (`cross`) in 0.7 s attacked in 3 of 3 loops (hand
    // 2.18 m/s, blade 3.1 rad/s at the window). The blade trails the hand (pommel first) while the
    // hand stays slower than a strike: a recovery, at 72, 90 and 120 Hz, and the same for the other
    // ways a raised or wound-up weapon is brought back.
    struct Case {
        std::string_view from, to;
        float seconds;
    };
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        for (const Case c :
             {Case{"high", "cross", 0.7f}, Case{"high", "cross", 0.6f}, Case{"high", "rest", 0.7f},
              Case{"high", "guard", 0.6f}, Case{"high", "blade_level", 0.6f},
              Case{"over_shoulder", "guard", 0.7f}, Case{"over_shoulder", "guard", 0.5f},
              Case{"behind_head", "guard", 0.7f}, Case{"behind_head", "cross", 0.7f},
              Case{"over_shoulder", "rest", 0.7f}}) {
            Sim sim;
            sim.Run(Move(c.from, c.to, c.seconds, hz), hz);
            EXPECT_EQ(sim.opened, 0)
                << c.from << " -> " << c.to << " " << c.seconds << " s @" << hz;
            EXPECT_EQ(sim.committed, 0)
                << c.from << " -> " << c.to << " " << c.seconds << " s @" << hz;
        }
    }
    // The QA's case itself is caught as a recovery stroke (not merely too slow).
    Sim qa;
    qa.Run(Move("high", "cross", 0.7f, 90.0f));
    EXPECT_GE(qa.recovery_strokes, 1);
}

TEST(MeleeWindowClassifier, TheQaPoseTimelineNeverAttacks) {
    // The QA's whole non-swing timeline (runs/c5: the right hand through rest, still, guard, blade
    // level, reach, out of reach, high, cross, wrist inject, tracking lost, roll sweep; each
    // reached in 0.7 s and held for the rest of its 3 s), three loops (99 s): no window, no commit.
    for (const float hz : {90.0f, 120.0f}) {
        Sim sim;
        sim.Run(PresetPath({"rest", "still", "guard", "blade_level", "reach_fwd", "out_of_reach",
                            "high", "cross", "wrist_inject", "lost", "roll_sweep"},
                           0.7f, 3, hz),
                hz);
        EXPECT_EQ(sim.opened, 0) << hz;
        EXPECT_EQ(sim.committed, 0) << hz;
    }
}

TEST(MeleeWindowClassifier, ChopsStillAttack) {
    // A chop wound up over the shoulder, down through the front (170 deg): at 0.35, 0.5 and 0.7 s
    // (hand peaks about 8.7, 6.1 and 4.4 m/s), each opens its window and commits.
    const M::V3 up_back{0.0f, 0.86f, 0.51f};
    for (const float seconds : {0.35f, 0.5f, 0.7f}) {
        Sim sim;
        sim.Run(Stroke(up_back, {-1.0f, 0.0f, 0.0f}, 170.0f * Pi / 180.0f, seconds, 0.3f));
        EXPECT_GE(sim.opened, 1) << seconds;
        EXPECT_EQ(sim.committed, 1) << seconds;
        EXPECT_EQ(sim.recovery_strokes, 0) << seconds;
    }
    // A diagonal chop from high right down to the left (140 deg in 0.4 s).
    Sim diag;
    diag.Run(Stroke({0.3f, 0.9f, 0.3f}, {-1.0f, 0.0f, -0.6f}, 140.0f * Pi / 180.0f, 0.4f, 0.3f));
    EXPECT_GE(diag.opened, 1);
    EXPECT_EQ(diag.committed, 1);
    // A chop whose wrist lags: the hand comes over the shoulder with the blade still pointing back
    // (60 deg behind the arm, the pommel first) and whips through at the end. Recovery-like while
    // the hand is slow; a strike once it reaches a strike's speed.
    {
        const M::V3 shoulder{0.18f, 1.38f, 0.05f};
        const float dt = 1.0f / 90.0f;
        const float seconds = 0.4f, arc = 150.0f * Pi / 180.0f, lag = 60.0f * Pi / 180.0f;
        std::vector<Sample> s;
        M::V3 prev{};
        for (int i = 0; i * dt < seconds + 0.6f; ++i) {
            const float t = i * dt - 0.3f;
            const float k = MinJerk(t / seconds);
            const float whip = MinJerk((t - 0.5f * seconds) / (0.5f * seconds));
            // The arm's angle down from straight up (about +x, to the front), the blade behind it.
            const float arm = arc * k;
            const float blade = arm - lag * (1.0f - whip);
            const M::V3 a{0.0f, std::cos(arm), -std::sin(arm)};
            Sample x{};
            x.p = shoulder + a * 0.55f;
            x.q = AxisAngle({-1.0f, 0.0f, 0.0f}, blade - 0.5f * Pi);
            x.v = i > 0 ? (x.p - prev) * (1.0f / dt) : M::V3{};
            prev = x.p;
            s.push_back(x);
        }
        Sim lagged;
        lagged.Run(s);
        EXPECT_GE(lagged.opened, 1);
        EXPECT_EQ(lagged.committed, 1);
    }
    // Lowering the arm across the body fast (high -> cross in 0.35 s, hand about 5 m/s): a
    // strike's speed, so a chop.
    Sim fast;
    fast.Run(Move("high", "cross", 0.35f, 90.0f));
    EXPECT_GE(fast.opened, 1);
    EXPECT_EQ(fast.committed, 1);
}

// ---- Overhead chops (QA 2026-10-07) -------------------------------------------------------------

M::V3 Elev(float deg) {
    return {0.0f, std::sin(deg * Pi / 180.0f), -std::cos(deg * Pi / 180.0f)};
}

// The grip whose -Z is `blade` (the shortest turn from -Z).
M::Q4 GripAlong(M::V3 blade) {
    const M::V3 z{0.0f, 0.0f, -1.0f};
    const M::V3 b = M::Normalize(blade);
    const M::V3 c = M::Cross(z, b);
    if (M::Length(c) < 1e-6f) {
        return M::Dot(z, b) > 0.0f ? M::Q4{} : M::Q4{1.0f, 0.0f, 0.0f, 0.0f};
    }
    return AxisAngle(c, std::atan2(M::Length(c), M::Dot(z, b)));
}

const M::V3 Eyes{0.0f, 1.6f, 0.0f};

// A chop from `from` (relative to the eyes, the blade `e0` degrees up from forward) to `to`, the
// blade turning down by `arc` degrees: the hand on a curve bulging forward, both minimum jerk over
// `seconds` after 0.3 s still, the blade `lag` s behind the hand; 0.5 s still after.
std::vector<Sample> Chop(M::V3 from, M::V3 to, float e0, float arc, float seconds, float lag,
                         float hz) {
    std::vector<Sample> out;
    const float dt = 1.0f / hz;
    const M::V3 bulge = (from + to) * 0.5f + M::V3{0.0f, 0.05f, -0.15f};
    M::V3 prev{};
    for (int i = 0; i * dt < 0.3f + seconds + lag + 0.5f; ++i) {
        const float t = i * dt;
        const float k = MinJerk((t - 0.3f) / seconds);
        const float kb = MinJerk((t - 0.3f - lag) / seconds);
        Sample s{};
        s.p = Eyes + from * ((1.0f - k) * (1.0f - k)) + bulge * (2.0f * (1.0f - k) * k) +
              to * (k * k);
        s.q = GripAlong(Elev(e0 - arc * kb));
        s.v = i > 0 ? (s.p - prev) * hz : M::V3{};
        prev = s.p;
        out.push_back(s);
    }
    return out;
}

// A path through keys (seconds, the hand relative to the eyes, the blade): Catmull-Rom with
// non-uniform times, still before the first key and after the last (0.5 s).
struct PathKey {
    float t;
    M::V3 p, b;
};

M::V3 KeyedAt(const std::vector<PathKey>& k, float t, bool blade) {
    const auto val = [&](std::size_t i) { return blade ? k[i].b : k[i].p; };
    const std::size_t n = k.size();
    if (t <= k[0].t) {
        return val(0);
    }
    if (t >= k[n - 1].t) {
        return val(n - 1);
    }
    std::size_t i = 0;
    while (i + 1 < n && k[i + 1].t < t) {
        ++i;
    }
    const float t0 = k[i].t, t1 = k[i + 1].t, s = (t - t0) / (t1 - t0);
    const auto tangent = [&](std::size_t j) {
        return j == 0 || j + 1 >= n
                   ? M::V3{}
                   : (val(j + 1) - val(j - 1)) * (1.0f / (k[j + 1].t - k[j - 1].t));
    };
    const M::V3 m0 = tangent(i) * (t1 - t0), m1 = tangent(i + 1) * (t1 - t0);
    const float s2 = s * s, s3 = s2 * s;
    return val(i) * (2 * s3 - 3 * s2 + 1) + m0 * (s3 - 2 * s2 + s) +
           val(i + 1) * (-2 * s3 + 3 * s2) + m1 * (s3 - s2);
}

std::vector<Sample> Keyed(const std::vector<PathKey>& keys, float hz) {
    std::vector<Sample> out;
    const float dt = 1.0f / hz;
    M::V3 prev{};
    for (int i = 0; i * dt < keys.back().t + 0.5f; ++i) {
        const float t = i * dt;
        Sample s{};
        s.p = Eyes + KeyedAt(keys, t, false);
        s.q = GripAlong(KeyedAt(keys, t, true));
        s.v = i > 0 ? (s.p - prev) * hz : M::V3{};
        prev = s.p;
        out.push_back(s);
    }
    return out;
}

std::vector<PathKey> Timed(std::vector<PathKey> keys, float t0, float k) {
    const float first = keys.front().t;
    for (PathKey& key : keys) {
        key.t = t0 + (key.t - first) * k;
    }
    return keys;
}

const M::V3 GuardRel{0.22f, -0.35f, -0.35f};

// Wound up out to the right, behind the shoulder and over the top into the chop in one motion
// (the blade's axis turns, never reverses), as the session's lost overhead swings.
std::vector<PathKey> LoopedChop() {
    return {{0.00f, GuardRel, Elev(40)},
            {0.20f, {0.38f, -0.05f, -0.15f}, M::Normalize({0.9f, 0.6f, -0.2f})},
            {0.36f, {0.28f, 0.18f, 0.05f}, M::Normalize({0.4f, 0.5f, 0.8f})},
            {0.46f, {0.16f, 0.26f, 0.0f}, Elev(125)},
            {0.58f, {0.15f, 0.0f, -0.42f}, Elev(30)},
            {0.68f, {0.12f, -0.45f, -0.38f}, Elev(-40)}};
}

// The session's window #17: cocked back slowly (60 deg in 250 ms), on round behind the head and
// over into a fast chop, one 235 deg stroke.
std::vector<PathKey> SlowCockedChop() {
    return {{0.00f, GuardRel, Elev(40)},
            {0.25f, {0.26f, -0.15f, -0.20f}, Elev(100)},
            {0.50f, {0.24f, 0.10f, 0.02f}, M::Normalize({0.45f, 0.4f, 0.8f})},
            {0.68f, {0.14f, 0.24f, 0.04f}, Elev(135)},
            {0.80f, {0.16f, -0.02f, -0.40f}, Elev(40)},
            {0.90f, {0.13f, -0.42f, -0.38f}, Elev(-35)}};
}

bool OpenAt(const Sim& sim, float t, float hz) {
    const std::size_t i = static_cast<std::size_t>(std::lround(t * hz));
    return i < sim.open_frames.size() && sim.open_frames[i];
}

TEST(MeleeWindowClassifier, OverheadChopsCommitAndAreOpenThroughTheirMiddle) {
    // From above the head, beside it (in the face zone), behind it and out at the shoulder,
    // down to the chest: the blade 100 or 140 deg up turning 80 or 120 deg, in 0.15 .. 0.35 s,
    // the blade with the hand or 60 ms behind it (still cocking back as the hand comes down), at
    // 72, 90 and 120 Hz with the face gate: one window, committed, open at the chop's middle.
    struct Start {
        const char* name;
        M::V3 p;
    };
    const Start starts[] = {{"above", {0.15f, 0.25f, 0.0f}},
                            {"beside", {0.20f, 0.05f, 0.02f}},
                            {"behind-high", {0.12f, 0.18f, 0.10f}},
                            {"shoulder-out", {0.30f, 0.10f, 0.0f}}};
    int n = 0, ok = 0, ok_before = 0;
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        for (const Start& st : starts) {
            for (const float e0 : {100.0f, 140.0f}) {
                for (const float arc : {80.0f, 120.0f}) {
                    for (const float T : {0.15f, 0.25f, 0.35f}) {
                        for (const float lag : {0.0f, 0.06f}) {
                            const auto path =
                                Chop(st.p, {0.15f, -0.40f, -0.40f}, e0, arc, T, lag, hz);
                            const float middle = 0.3f + 0.5f * T + lag;
                            Sim sim;
                            sim.face = true;
                            sim.Run(path, hz);
                            ++n;
                            const bool good = sim.committed == 1 && OpenAt(sim, middle, hz);
                            ok += good ? 1 : 0;
                            EXPECT_TRUE(good)
                                << st.name << " e" << e0 << " arc " << arc << " " << T << " s lag "
                                << lag << " @" << hz << ": committed " << sim.committed
                                << " open at the middle " << OpenAt(sim, middle, hz);
                            Sim before;
                            before.face = true;
                            before.face_rule.enter_speed = 0.0f;
                            before.overhead = false;
                            before.Run(path, hz);
                            ok_before +=
                                before.committed == 1 && OpenAt(before, middle, hz) ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    std::printf("overhead chops: %d of %d committed and open at their middle (before: %d)\n", ok, n,
                ok_before);
}

TEST(MeleeWindowClassifier, TheSessionsLoopedChopsCommit) {
    // A wind-up looped into its chop (and the session's window #17): the strike opens its window,
    // commits and is open through the chop's middle; the old rules lost it (the onset window
    // aborted "not a swing (ignored)", no commit).
    struct Case {
        const char* name;
        std::vector<PathKey> keys;
        float middle; // the chop's middle (the keys' time)
    };
    const Case cases[] = {{"looped over the shoulder", LoopedChop(), 0.58f},
                          {"window #17", SlowCockedChop(), 0.80f}};
    for (const Case& c : cases) {
        for (const float hz : {72.0f, 90.0f, 120.0f}) {
            for (const float k : {0.7f, 1.0f, 1.4f}) {
                const auto keys = Timed(c.keys, 0.3f, k);
                const float middle = 0.3f + c.middle * k;
                Sim sim;
                sim.face = true;
                sim.Run(Keyed(keys, hz), hz);
                EXPECT_EQ(sim.committed, 1) << c.name << " x" << k << " @" << hz;
                EXPECT_TRUE(OpenAt(sim, middle, hz)) << c.name << " x" << k << " @" << hz;
                if (k > 1.0f) {
                    continue; // (slower, the old rules split it at a pause too)
                }
                Sim before;
                before.face = true;
                before.face_rule.enter_speed = 0.0f;
                before.overhead = false;
                before.Run(Keyed(keys, hz), hz);
                EXPECT_EQ(before.committed, 0) << c.name << " x" << k << " @" << hz << " before";
            }
        }
    }
}

TEST(MeleeWindowClassifier, WindUpsRaisesAndTheHeadsetNeverAttack) {
    for (const float hz : {72.0f, 90.0f, 120.0f}) {
        // The looped wind-up held at the top; then brought back down to the guard in 0.8 s.
        for (const float k : {0.7f, 1.0f, 1.4f}) {
            const std::vector<PathKey> loop = LoopedChop();
            std::vector<PathKey> keys = Timed(
                {loop[0], loop[1], loop[2], {0.48f, {0.16f, 0.26f, 0.04f}, Elev(135)}}, 0.3f, k);
            Sim held;
            held.face = true;
            held.Run(Keyed(keys, hz), hz);
            EXPECT_EQ(held.committed, 0) << "held x" << k << " @" << hz;
            const PathKey top = keys.back();
            keys.push_back({top.t + 0.3f, top.p, top.b});
            keys.push_back({top.t + 1.1f, GuardRel, Elev(40)});
            Sim lowered;
            lowered.face = true;
            lowered.Run(Keyed(keys, hz), hz);
            EXPECT_EQ(lowered.committed, 0) << "lowered x" << k << " @" << hz;
        }
        // Raised slowly from the guard over the shoulder (0.8 s) and behind the head (1.0 s).
        for (const auto& [to, seconds] :
             {std::pair{"over_shoulder", 0.8f}, std::pair{"behind_head", 1.0f}}) {
            Sim raise;
            raise.face = true;
            raise.Run(Move("guard", to, seconds, hz), hz);
            EXPECT_EQ(raise.opened, 0) << to << " @" << hz;
            EXPECT_EQ(raise.committed, 0) << to << " @" << hz;
        }
        // The hand brought to the visor (0.4 s), held there 0.5 s, then lowered to the guard in
        // 0.5 s: swings off at the face, no window.
        const std::vector<PathKey> visor{{0.0f, GuardRel, Elev(40)},
                                         {0.4f, {0.05f, 0.0f, -0.10f}, Elev(80)},
                                         {0.9f, {0.05f, 0.0f, -0.10f}, Elev(80)},
                                         {1.4f, GuardRel, Elev(40)}};
        Sim face;
        face.face = true;
        face.Run(Keyed(Timed(visor, 0.3f, 1.0f), hz), hz);
        EXPECT_GE(face.face_offs, 1) << hz;
        EXPECT_EQ(face.committed, 0) << hz;
    }
}

TEST(MeleeFace, ASwingPassingTheFaceIsNoHandAtTheHeadset) {
    M::FaceRule rule{};
    // In the zone (15 cm in front of the eyes) at swing speed: not at the face; slow: at the face.
    EXPECT_FALSE(M::HandAtFace(false, 0.10f, 0.15f, 4.0f, rule));
    EXPECT_TRUE(M::HandAtFace(false, 0.10f, 0.15f, 0.5f, rule));
    // Once at the face it stays until past the exit, at any speed (lowered from the headset).
    EXPECT_TRUE(M::HandAtFace(true, 0.10f, 0.24f, 3.0f, rule));
    EXPECT_FALSE(M::HandAtFace(true, 0.10f, 0.27f, 0.2f, rule));
    // Behind the eyes (an over-the-shoulder wind-up): never.
    EXPECT_FALSE(M::HandAtFace(false, -0.08f, 0.15f, 0.0f, rule));
    // swing_face_enter_speed=0: the old rule, any speed.
    rule.enter_speed = 0.0f;
    EXPECT_TRUE(M::HandAtFace(false, 0.10f, 0.15f, 4.0f, rule));
    // A chop from behind the head passing 12-19 cm in front of the eyes at 4-9 m/s never turns
    // swings off; the same chop with the old rule does.
    for (const float T : {0.15f, 0.25f}) {
        const auto path =
            Chop({0.12f, 0.18f, 0.10f}, {0.18f, -0.25f, -0.45f}, 60.0f, 80.0f, T, 0.0f, 90.0f);
        Sim sim;
        sim.face = true;
        sim.Run(path);
        EXPECT_EQ(sim.face_offs, 0) << T;
        EXPECT_EQ(sim.committed, 1) << T;
        Sim before;
        before.face = true;
        before.face_rule.enter_speed = 0.0f;
        before.Run(path);
        EXPECT_GE(before.face_offs, 1) << T;
    }
}

// ---- The telegraph ------------------------------------------------------------------------------

struct TelegraphRig {
    M::TelegraphTracker tracker{};
    M::TelegraphConfig cfg{};
    M::Window w{};
    M::TelegraphInput in{};
    float t = 0.0f;

    TelegraphRig() {
        in.dt = 1.0f / 90.0f;
        in.swings = true;
        in.window = &w;
    }
    // One frame; the number of telegraphs it gave (0 or 1) and why.
    int Step(const char** why = nullptr) {
        t += in.dt;
        const M::TelegraphOutput o = tracker.Step(in, cfg);
        in.opened = false;
        if (o.fire && why != nullptr) {
            *why = o.why;
        }
        return o.fire ? 1 : 0;
    }
    int Run(float seconds) {
        int n = 0;
        for (float s = 0.0f; s < seconds; s += in.dt) {
            n += Step();
        }
        return n;
    }
    void Open(M::MoveKind kind, unsigned stroke) {
        w = {};
        w.serial = ++serial;
        w.open = true;
        w.kind = kind;
        w.stroke = stroke;
        in.opened = true;
    }
    void Close() {
        w.open = false;
    }
    std::uint32_t serial = 0;
};

TEST(MeleeTelegraph, AQuickLightFlickNeverTells) {
    int fired = 0; // (each call once: EXPECT may evaluate twice)
    TelegraphRig r;
    r.in.stroke_live = true;
    r.in.stroke = 1;
    r.in.stroke_arc = 90.0f * Pi / 180.0f;
    r.Open(M::MoveKind::Light, 1);
    fired = r.Run(0.30f); // a 0.3 s light window, 90 deg: nothing
    EXPECT_EQ(fired, 0);
    r.Close();
    r.in.stroke_live = false;
    fired = r.Run(1.0f);
    EXPECT_EQ(fired, 0);
}

TEST(MeleeTelegraph, AWindUpTellsOnceForItsStrike) {
    int fired = 0; // (each call once: EXPECT may evaluate twice)
    TelegraphRig r;
    // Raising the blade: a wind-up stroke, 40 deg (not yet), then 70 deg.
    r.in.stroke_live = true;
    r.in.stroke = 1;
    r.in.windup = "raising";
    r.in.blade_angle = 110.0f * Pi / 180.0f;
    r.in.stroke_arc = 40.0f * Pi / 180.0f;
    fired = r.Run(0.1f);
    EXPECT_EQ(fired, 0);
    r.in.stroke_arc = 70.0f * Pi / 180.0f;
    const char* why = "";
    fired = r.Step(&why);
    EXPECT_EQ(fired, 1);
    EXPECT_STREQ(why, "a wind-up (raised)");
    // Held up, then the heavy strike's window (0.5 s later) and its 0.4 s: no second notice.
    r.in.stroke_live = false;
    r.in.windup = nullptr;
    r.in.wound_up = true;
    fired = r.Run(0.5f);
    EXPECT_EQ(fired, 0);
    r.in.wound_up = false;
    r.in.stroke_live = true;
    r.in.stroke = 2;
    r.in.stroke_arc = 0.0f;
    r.Open(M::MoveKind::Heavy, 2);
    fired = r.Run(0.45f); // (heavy, and long: the wind-up's notice covers it)
    EXPECT_EQ(fired, 0);
    r.Close();
    r.in.stroke_live = false;
    fired = r.Run(1.0f);
    EXPECT_EQ(fired, 0);
    // A new heavy later tells again.
    r.Open(M::MoveKind::Heavy, 3);
    fired = r.Step(&why);
    EXPECT_EQ(fired, 1);
    EXPECT_STREQ(why, "a heavy swing");
}

TEST(MeleeTelegraph, HeavyChargedLongAndWideSwingsTell) {
    int fired = 0; // (each call once: EXPECT may evaluate twice)
    for (const M::MoveKind k : {M::MoveKind::Heavy, M::MoveKind::Charged}) {
        TelegraphRig r;
        r.in.stroke_live = true;
        r.in.stroke = 1;
        r.Open(k, 1);
        const char* why = "";
        fired = r.Step(&why);
        EXPECT_EQ(fired, 1);
        EXPECT_STREQ(why, k == M::MoveKind::Heavy ? "a heavy swing" : "a charged swing");
    }
    // A light window still open after melee_telegraph_long_s.
    TelegraphRig l;
    l.in.stroke_live = true;
    l.in.stroke = 1;
    l.Open(M::MoveKind::Light, 1);
    fired = l.Run(l.cfg.long_s - 0.03f);
    EXPECT_EQ(fired, 0);
    const char* why = "";
    int n = 0;
    for (int i = 0; i < 6; ++i) {
        n += l.Step(&why);
    }
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(why, "a long swing");
    // A light stroke sweeping the heavy arc.
    TelegraphRig wide;
    wide.in.stroke_live = true;
    wide.in.stroke = 4;
    wide.Open(M::MoveKind::Light, 4);
    wide.in.stroke_arc = 100.0f * Pi / 180.0f;
    fired = wide.Run(0.1f);
    EXPECT_EQ(fired, 0);
    wide.in.stroke_arc = 150.0f * Pi / 180.0f;
    fired = wide.Step(&why);
    EXPECT_EQ(fired, 1);
    EXPECT_STREQ(why, "a wide swing");
}

TEST(MeleeTelegraph, ModesAndGates) {
    int fired = 0; // (each call once: EXPECT may evaluate twice)
    // all: every swing; off: none; swings off: none.
    TelegraphRig all;
    all.cfg.mode = M::TelegraphMode::All;
    all.Open(M::MoveKind::Light, 1);
    const char* why = "";
    fired = all.Step(&why);
    EXPECT_EQ(fired, 1);
    EXPECT_STREQ(why, "a swing");
    TelegraphRig off;
    off.cfg.mode = M::TelegraphMode::Off;
    off.Open(M::MoveKind::Heavy, 1);
    fired = off.Run(1.0f);
    EXPECT_EQ(fired, 0);
    TelegraphRig gated;
    gated.in.swings = false;
    gated.Open(M::MoveKind::Charged, 1);
    fired = gated.Run(1.0f);
    EXPECT_EQ(fired, 0);
    // Raised slowly into a wound-up hold (no stroke): tells; the same pose reached at the end of
    // a stroke (a follow-through) does not.
    TelegraphRig pose;
    fired = pose.Run(0.1f);
    EXPECT_EQ(fired, 0);
    pose.in.wound_up = true;
    fired = pose.Step(&why);
    EXPECT_EQ(fired, 1);
    EXPECT_STREQ(why, "the weapon raised into a wound-up hold");
    TelegraphRig follow;
    follow.in.stroke_live = true;
    follow.in.stroke = 1;
    fired = follow.Run(0.1f);
    EXPECT_EQ(fired, 0);
    follow.in.wound_up = true;
    fired = follow.Run(0.2f);
    EXPECT_EQ(fired, 0);
    follow.in.stroke_live = false;
    fired = follow.Run(1.0f);
    EXPECT_EQ(fired, 0);
}

TEST(MeleeTelegraph, BringingTheWeaponBackToGuardIsNoWindUp) {
    int fired = 0; // (each call once: EXPECT may evaluate twice)
    // After a chop the blade comes back up 80 deg, to 45 deg from the front: no notice; the same
    // raise on to 110 deg (over the shoulder): one.
    TelegraphRig r;
    r.in.stroke_live = true;
    r.in.stroke = 1;
    r.in.windup = "raising";
    r.in.stroke_arc = 80.0f * Pi / 180.0f;
    r.in.blade_angle = 45.0f * Pi / 180.0f;
    fired = r.Run(0.3f);
    EXPECT_EQ(fired, 0);
    r.in.blade_angle = 110.0f * Pi / 180.0f;
    fired = r.Run(0.1f);
    EXPECT_EQ(fired, 1);
}

TEST(MeleeTelegraph, NoNoticeSoonerThanTheGap) {
    // Wind-ups every 0.3 s: one notice per TelegraphGap (re-arming the AI's timer every few frames
    // would keep its FindAttack from ever firing).
    TelegraphRig r;
    r.in.windup = "cocking";
    r.in.blade_angle = 120.0f * Pi / 180.0f;
    r.in.stroke_arc = 80.0f * Pi / 180.0f;
    int n = 0;
    for (int k = 0; k < 10; ++k) {
        r.in.stroke_live = true;
        r.in.stroke = static_cast<unsigned>(k + 1);
        for (int i = 0; i < 27; ++i) {
            n += r.Step();
        }
    }
    const float seconds = 10.0f * 27.0f / 90.0f;
    EXPECT_LE(n, static_cast<int>(seconds / r.cfg.gap_s) + 1);
    EXPECT_GE(n, 3);
}

TEST(MeleeTelegraphClassifier, AQuickForehandIsUnreadableAWindUpAndChopIsRead) {
    // The quick forehand from guard (120 deg in 0.35 s): no notice.
    const M::V3 start{0.96f, 0.0f, -0.26f};
    Sim quick;
    quick.Run(Stroke(start, {0.0f, 1.0f, 0.0f}, 120.0f * Pi / 180.0f, 0.35f, 0.4f));
    EXPECT_EQ(quick.committed, 1);
    EXPECT_EQ(quick.telegraphs, 0) << quick.telegraph_why;
    // ... with melee_telegraph=all: one.
    Sim quick_all;
    quick_all.gcfg.mode = M::TelegraphMode::All;
    quick_all.Run(Stroke(start, {0.0f, 1.0f, 0.0f}, 120.0f * Pi / 180.0f, 0.35f, 0.4f));
    EXPECT_EQ(quick_all.telegraphs, 1);
    // Cocked back over the shoulder (130 deg up and back in 0.35 s), then chopped down 170 deg in
    // 0.3 s: one notice, during the wind-up, before the strike's window opens.
    std::vector<Sample> s =
        Stroke({0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, 130.0f * Pi / 180.0f, 0.35f, 0.3f);
    const M::V3 up{0.0f, std::sin(130.0f * Pi / 180.0f), -std::cos(130.0f * Pi / 180.0f)};
    const std::vector<Sample> chop =
        Stroke(up, {1.0f, 0.0f, 0.0f}, -170.0f * Pi / 180.0f, 0.3f, 0.15f);
    s.insert(s.end(), chop.begin(), chop.end());
    Sim read;
    read.Run(s);
    EXPECT_EQ(read.telegraphs, 1);
    EXPECT_GE(read.opened, 1);
    ASSERT_GE(read.first_telegraph_t, 0.0f);
    EXPECT_LT(read.first_telegraph_t, read.first_open_t);
    EXPECT_EQ(read.first_kind, M::MoveKind::Heavy);
    std::printf("wind-up and chop: notice at %.0f ms (%s), the strike's window at %.0f ms (%s)\n",
                read.first_telegraph_t * 1000.0f, read.telegraph_why, read.first_open_t * 1000.0f,
                M::MoveKindName(read.first_kind));
}

TEST(MeleeMoves, TheWeaponsOwnSoundAndTrail) {
    // The Saw Cleaver's R1 clip a112 30000: whoosh 610, FFX 250 from dummy 110 to 100 (form A);
    // the Hunter Axe's a103: 760, FFX 251; the Blade of Mercy's a113 (its group matched by its
    // rows): 5000, FFX 250 from 120 to 100; Ludwig's form A the Kirkhammer's a105 (783, FFX 253),
    // its form B its own a120 (810); the Kos Parasite (no clip known) keeps the defaults (-1).
    const M::WeaponMoves& saw = Moves(700);
    EXPECT_EQ(saw.rows[0].whoosh, 610);
    EXPECT_EQ(saw.rows[0].trail_ffx, 250);
    EXPECT_EQ(saw.rows[0].trail_base, 10110);
    EXPECT_EQ(saw.rows[0].trail_tip, 10100);
    EXPECT_EQ(saw.rows[3].whoosh, 611);
    const M::WeaponMoves& axe = Moves(500);
    EXPECT_EQ(axe.rows[0].whoosh, 760);
    EXPECT_EQ(axe.rows[0].trail_ffx, 251);
    const M::WeaponMoves& mercy = Moves(400);
    EXPECT_EQ(mercy.rows[0].whoosh, 5000);
    EXPECT_EQ(mercy.rows[0].trail_ffx, 250);
    EXPECT_EQ(mercy.rows[0].trail_base, 10120);
    // Its form-B R2's clip trails the LEFT blade (13120 -> 13100): not the right blade's swing.
    EXPECT_EQ(mercy.rows[4].trail_ffx, -1);
    EXPECT_EQ(Moves(810).rows[0].whoosh, 783);
    EXPECT_EQ(Moves(810).rows[0].trail_ffx, 253);
    EXPECT_EQ(Moves(810).rows[3].whoosh, 810);
    EXPECT_EQ(Moves(101).rows[0].whoosh, -1);
    EXPECT_EQ(Moves(101).rows[0].trail_ffx, -1);
    // Every trail found is on the row's own part (its dummy group) and never a 4xxx voice.
    for (const M::WeaponMoves& w : M::Moves) {
        for (const M::MoveRow& r : w.rows) {
            if (!r.Valid()) {
                continue;
            }
            EXPECT_FALSE(r.whoosh >= 4000 && r.whoosh <= 4999) << w.name;
            if (r.trail_ffx >= 0) {
                EXPECT_EQ(r.trail_base / 1000, r.Group()) << w.name;
                EXPECT_EQ(r.trail_tip / 1000, r.Group()) << w.name;
            }
        }
    }
}

// ---- A flick mid-swing: the transform attack ------------------------------------------------------

TEST(MeleeFlick, TheTransformAttackRows) {
    // The game's own trick attack of each weapon (judge 390 A->B, 790 B->A), hitting with the part
    // shown after the switch (A->B: the form-B groups 11-13, B->A: group 10).
    for (const M::TransformMoveRows& m : M::TransformMoves) {
        EXPECT_NE(M::FindMoves(m.variation), nullptr) << m.variation;
        for (int dir = 0; dir < 2; ++dir) {
            const M::MoveRow& r = m.rows[dir];
            if (!r.Valid()) {
                continue;
            }
            EXPECT_EQ(r.judge, dir == 0 ? 390 : 790) << m.variation;
            if (dir == 0) {
                EXPECT_GE(r.Group(), 11) << m.variation;
                EXPECT_LE(r.Group(), 13) << m.variation;
            } else {
                EXPECT_EQ(r.Group(), 10) << m.variation;
            }
            EXPECT_GE(r.mv, 50) << m.variation;
            EXPECT_GE(r.radius, 0.05f) << m.variation;
            EXPECT_GT(r.stamina, 0) << m.variation;
            if (r.trail_ffx >= 0) {
                EXPECT_EQ(r.trail_base / 1000, r.Group()) << m.variation;
                EXPECT_EQ(r.trail_tip / 1000, r.Group()) << m.variation;
            }
        }
    }
    // The kit's weapons: the Saw Cleaver and the Hunter Axe both ways, the Threaded Cane into the
    // whip (folding back, the game's trick attack hits with the whip before it folds: none).
    const M::MoveRow* saw_b = M::FindTransformMove(700, true);
    const M::MoveRow* saw_a = M::FindTransformMove(700, false);
    ASSERT_NE(saw_b, nullptr);
    ASSERT_NE(saw_a, nullptr);
    EXPECT_EQ(saw_b->dmy1, 12100);
    EXPECT_EQ(saw_a->dmy1, 10100);
    EXPECT_GT(saw_b->mv, Moves(700).rows[3].mv); // stronger than form B's light row
    EXPECT_GT(saw_a->mv, Moves(700).rows[0].mv);
    EXPECT_NE(M::FindTransformMove(500, true), nullptr);
    EXPECT_NE(M::FindTransformMove(500, false), nullptr);
    const M::MoveRow* cane_b = M::FindTransformMove(2200, true);
    ASSERT_NE(cane_b, nullptr);
    EXPECT_EQ(cane_b->Group(), 12);
    EXPECT_EQ(M::FindTransformMove(2200, false), nullptr);
    EXPECT_EQ(M::FindTransformMove(-1, true), nullptr);
    EXPECT_EQ(M::FindTransformMove(12345, true), nullptr);
}

TEST(MeleeFlick, WhichStrokeAFlickJoins) {
    constexpr std::int64_t Ms = 1'000'000;
    M::FlickJoinInput in{};
    in.enabled = true;
    in.window = 7;
    in.open = true;
    in.open_ns = 1000 * Ms;
    in.run = 3;
    in.run_active = true;
    in.press_ns = 1100 * Ms; // squeezed 100 ms into the stroke
    in.now = 1120 * Ms;
    EXPECT_TRUE(M::FlickJoins(in));
    // Off, no window, no run, or a run too old.
    M::FlickJoinInput off = in;
    off.enabled = false;
    EXPECT_FALSE(M::FlickJoins(off));
    M::FlickJoinInput none = in;
    none.window = 0;
    EXPECT_FALSE(M::FlickJoins(none));
    none = in;
    none.run = 0;
    EXPECT_FALSE(M::FlickJoins(none));
    M::FlickJoinInput old = in;
    old.now = in.press_ns + 1600 * Ms;
    EXPECT_FALSE(M::FlickJoins(old));
    // A stroke over before the squeeze is not the flick's.
    M::FlickJoinInput over = in;
    over.open = false;
    over.close_ns = 1050 * Ms;
    EXPECT_FALSE(M::FlickJoins(over));
    // One closed after the squeeze is (its attack ends as the switch is handed over).
    over.close_ns = 1150 * Ms;
    over.now = 1160 * Ms;
    EXPECT_TRUE(M::FlickJoins(over));
    // A stroke that opens after the squeeze: while the transform has not switched yet, or within
    // the attach time (the squeeze came a moment before the stroke's start).
    M::FlickJoinInput later = in;
    later.open_ns = 1400 * Ms; // 300 ms after the squeeze
    later.now = 1400 * Ms;
    EXPECT_TRUE(M::FlickJoins(later));
    later.switched = true;
    EXPECT_TRUE(M::FlickJoins(later)); // 300 <= 350 ms
    later.open_ns = 1500 * Ms;           // 400 ms after it, switched: a new swing of its own
    later.now = 1500 * Ms;
    EXPECT_FALSE(M::FlickJoins(later));
    later.switched = false; // ... unless the transform still runs before its switch
    EXPECT_TRUE(M::FlickJoins(later));
    later.run_active = false;
    EXPECT_FALSE(M::FlickJoins(later));
}

// ---- The combo budget ---------------------------------------------------------------------------

TEST(MeleeBudget, ThePaceTable) {
    // Every weapon of the move table has its paces; every move it has, a pace > 0.
    ASSERT_EQ(M::Paces.size(), M::Moves.size());
    for (const M::WeaponMoves& w : M::Moves) {
        const M::WeaponPace* p = M::FindPace(w.variation);
        ASSERT_NE(p, nullptr) << w.name;
        for (std::size_t i = 0; i < 6; ++i) {
            if (w.rows[i].Valid()) {
                EXPECT_GE(p->recover[i], 0.40f) << w.name << " row " << i;
                EXPECT_LE(p->recover[i], 2.2f) << w.name << " row " << i;
            }
        }
        // A form without any physical move has no pace (no swing there).
        EXPECT_EQ(p->recover[3] > 0.0f, w.rows[3].Valid()) << w.name;
    }
    // The game's own R1 cancel (TAE ChrActionFlag 4) of the first R1, R2 and charged R2.
    const M::WeaponPace* saw = M::FindPace(700);
    ASSERT_NE(saw, nullptr);
    EXPECT_NEAR(saw->recover[0], 0.567f, 1e-4f); // a112_030000
    EXPECT_NEAR(saw->recover[1], 0.867f, 1e-4f); // a112_032620
    EXPECT_NEAR(saw->recover[2], 0.933f, 1e-4f); // a112_032630
    EXPECT_NEAR(saw->recover[3], 0.767f, 1e-4f); // a112_040000 (form B)
    const M::WeaponPace* axe = M::FindPace(500);
    ASSERT_NE(axe, nullptr);
    EXPECT_NEAR(axe->recover[0], 0.767f, 1e-4f); // a103_030000
    EXPECT_NEAR(axe->recover[2], 1.300f, 1e-4f);
    const M::WeaponPace* mercy = M::FindPace(400);
    ASSERT_NE(mercy, nullptr);
    EXPECT_NEAR(mercy->recover[3], 0.467f, 1e-4f); // the fastest: a113_040000
    // A missing heavier move has its lighter move's pace (ChooseMove opens that one).
    const M::WeaponPace* simon = M::FindPace(3200);
    ASSERT_NE(simon, nullptr);
    EXPECT_EQ(simon->recover[2], simon->recover[1]);
    EXPECT_EQ(M::FindPace(1400), nullptr);
    // The refill: x scale + offset (the user's -80 ms, 2026-10-07: Saw Cleaver 0.487 s, Hunter
    // Axe 0.687 s; -50 ms on 10-06: 0.517 / 0.717; -20 ms before 2026-10-06: 0.547 / 0.747).
    const M::BudgetConfig c{};
    EXPECT_NEAR(c.offset_s, -0.080f, 1e-6f);
    EXPECT_NEAR(M::BudgetRefill(M::PaceRecover(saw, false, M::MoveKind::Light), c), 0.487f, 1e-4f);
    EXPECT_NEAR(M::BudgetRefill(M::PaceRecover(axe, false, M::MoveKind::Light), c), 0.687f, 1e-4f);
    EXPECT_NEAR(M::BudgetRefill(M::PaceRecover(saw, true, M::MoveKind::Heavy), c), 0.653f, 1e-4f);
    M::BudgetConfig old = c;
    old.offset_s = -0.020f; // melee_budget_offset_ms=-20: the refill before 2026-10-06
    EXPECT_NEAR(M::BudgetRefill(M::PaceRecover(saw, false, M::MoveKind::Light), old), 0.547f, 1e-4f);
    old.offset_s = -0.050f; // -50: the 2026-10-06 refill
    EXPECT_NEAR(M::BudgetRefill(M::PaceRecover(saw, false, M::MoveKind::Light), old), 0.517f, 1e-4f);
    M::BudgetConfig slow = c;
    slow.scale = 1.5f;
    slow.offset_s = 0.0f;
    EXPECT_NEAR(M::BudgetRefill(0.567f, slow), 0.8505f, 1e-4f);
    slow.scale = 0.0f;
    EXPECT_NEAR(M::BudgetRefill(0.567f, slow), slow.min_s, 1e-6f);
    // Unknown weapon or form without moves: the generic recovery.
    EXPECT_NEAR(M::PaceRecover(nullptr, false, M::MoveKind::Heavy),
                M::RecoverSeconds(M::MoveKind::Heavy), 1e-6f);
    EXPECT_NEAR(M::PaceRecover(M::FindPace(2200), true, M::MoveKind::Light),
                M::RecoverSeconds(M::MoveKind::Light), 1e-6f);
    // Costs: light 1, heavy 2, charged all.
    EXPECT_EQ(M::BudgetCost(M::MoveKind::Light, 2), 1);
    EXPECT_EQ(M::BudgetCost(M::MoveKind::Heavy, 2), 2);
    EXPECT_EQ(M::BudgetCost(M::MoveKind::Charged, 2), 2);
    EXPECT_EQ(M::BudgetCost(M::MoveKind::Heavy, 1), 1);
    EXPECT_EQ(M::BudgetCost(M::MoveKind::Charged, 3), 3);
}

TEST(MeleeBudget, ChargesComeBackOneBlockAtATime) {
    M::ComboBudget b{};
    b.SetCapacity(2);
    EXPECT_EQ(b.Available(0.0), 2);
    ASSERT_TRUE(b.Spend(1, 1, 0.547f, 0.0)); // a light
    EXPECT_EQ(b.Available(0.0), 1);
    ASSERT_TRUE(b.Spend(2, 1, 0.547f, 0.36)); // a quick second one
    EXPECT_EQ(b.Available(0.36), 0);
    EXPECT_FALSE(b.Spend(3, 1, 0.547f, 0.50)); // a third too soon
    EXPECT_NEAR(b.NextIn(0.50), 0.047f, 1e-4f);
    EXPECT_EQ(b.Available(0.546), 0);
    EXPECT_EQ(b.Available(0.548), 1); // the first one's back 0.547 s after it
    EXPECT_EQ(b.Available(1.093), 1); // the second's comes back after the first's (0.547 + 0.547)
    EXPECT_EQ(b.Available(1.095), 2);
    // A heavy uses both; they come back together after its own recovery.
    M::ComboBudget h{};
    ASSERT_TRUE(h.Spend(1, 2, 0.847f, 0.0));
    EXPECT_EQ(h.Available(0.5), 0);
    EXPECT_FALSE(h.Spend(2, 1, 0.547f, 0.5));
    EXPECT_EQ(h.Available(0.848), 2);
    // A light then a heavy: the heavy waits for the light's charge (the game's R1 -> R2).
    M::ComboBudget m{};
    ASSERT_TRUE(m.Spend(1, 1, 0.547f, 0.0));
    EXPECT_FALSE(m.Spend(2, 2, 0.847f, 0.3));
    EXPECT_TRUE(m.Spend(2, 2, 0.847f, 0.55));
    EXPECT_EQ(m.Available(1.39), 0);
    EXPECT_EQ(m.Available(1.40), 2);
    // Swinging at the game's own pace always has a charge; flailing gets that pace plus one.
    M::ComboBudget p{};
    int registered = 0;
    for (int i = 0; i < 20; ++i) {
        registered += p.Spend(static_cast<std::uint32_t>(i + 1), 1, 0.547f, i * 0.55) ? 1 : 0;
    }
    EXPECT_EQ(registered, 20);
    M::ComboBudget f{};
    registered = 0;
    for (int i = 0; i < 40; ++i) { // every 0.36 s for 14 s
        registered += f.Spend(static_cast<std::uint32_t>(i + 1), 1, 0.547f, i * 0.36) ? 1 : 0;
    }
    const int pace = static_cast<int>(39 * 0.36 / 0.547) + 1;
    EXPECT_GE(registered, pace);
    EXPECT_LE(registered, pace + 1);
    EXPECT_LT(registered, 40);
}

TEST(MeleeBudget, RefundAndRebill) {
    M::ComboBudget b{};
    ASSERT_TRUE(b.Spend(1, 1, 0.5f, 0.0));
    ASSERT_TRUE(b.Spend(2, 1, 0.5f, 0.1)); // queued behind the first: back at 1.0
    EXPECT_EQ(b.Held(2, 0.2), 1);
    // The first one given back at 0.2 (the game refused it): the second starts from its own swing.
    EXPECT_TRUE(b.Refund(1, 0.2));
    EXPECT_EQ(b.Available(0.2), 1);
    EXPECT_NEAR(b.NextIn(0.2), 0.4f, 1e-4f); // 0.1 + 0.5
    EXPECT_FALSE(b.Refund(1, 0.2));          // once
    EXPECT_FALSE(b.Refund(2, 0.7));          // back already
    // The head given back after it waited: the time goes to the next block.
    M::ComboBudget c{};
    ASSERT_TRUE(c.Spend(1, 1, 0.5f, 0.0));
    ASSERT_TRUE(c.Spend(2, 1, 0.5f, 0.0));
    EXPECT_TRUE(c.Refund(1, 0.3));
    EXPECT_NEAR(c.NextIn(0.3), 0.2f, 1e-4f);
    // A heavy judged light: one charge, the light's time, from when it was used.
    M::ComboBudget h{};
    ASSERT_TRUE(h.Spend(5, 2, 0.847f, 1.0));
    EXPECT_TRUE(h.Rebill(5, 1, 0.547f, 1.1));
    EXPECT_EQ(h.Available(1.1), 1);
    EXPECT_NEAR(h.NextIn(1.1), 0.447f, 1e-4f);
    EXPECT_FALSE(h.Rebill(6, 1, 0.547f, 1.1));
    // A new charge count starts full.
    h.SetCapacity(3);
    EXPECT_EQ(h.Available(1.1), 3);
}

// The rig's weapon and a swing: the stroke's start (an onset window), its commit 4 frames later,
// the blade slowing 10 frames after; the window and what the commit frame said.
struct Swung {
    M::Window w{};
    bool empty{false};
    bool paid_late{false};
    bool opened{false};
};

void Weapon(Rig& r, int variation, bool form_b = false) {
    const M::WeaponPace* p = M::FindPace(variation);
    for (std::size_t k = 0; k < 3; ++k) {
        r.in.recover[k] = M::PaceRecover(p, form_b, static_cast<M::MoveKind>(k));
    }
    // As the presenter: the heaviest move the weapon has in that form.
    if (const M::WeaponMoves* m = M::FindMoves(variation); m != nullptr) {
        const M::MoveChoice top = M::ChooseMove(*m, form_b, M::MoveKind::Charged);
        r.in.top = top.row != nullptr ? top.kind : M::MoveKind::Charged;
    }
}

Swung Swing(Rig& r, unsigned stroke, bool cocked = false, int commit_after = 4) {
    Swung s{};
    r.Fast(stroke, cocked);
    s.opened = r.Step().opened;
    for (int i = 1; i < commit_after; ++i) {
        r.Step();
    }
    r.in.fired = true;
    r.in.fired_stroke = stroke;
    const M::TrackerOutput& o = r.Step();
    s.empty = o.empty;
    s.paid_late = o.paid_late;
    for (int i = 0; i < 10; ++i) {
        r.Step();
    }
    r.Slow();
    for (int i = 0; i < 20 && r.W().open; ++i) {
        r.Step();
    }
    s.w = r.W();
    return s;
}

// Slow frames until the budget's clock reaches `t` (s).
void Until(Rig& r, double t) {
    r.Slow();
    while (r.tracker.BudgetClock() < t - 1e-6) {
        r.Step();
    }
}

TEST(MeleeBudget, TwoQuickSlashesRegisterTheThirdTooSoonDoesNot) {
    // The Hunter Axe (light 0.767 -> 0.717 s): strokes 0.36 and 0.27 s apart, each committing
    // 44 ms after its start.
    Rig r;
    Weapon(r, 500);
    const Swung a = Swing(r, 1);
    EXPECT_FALSE(a.w.unpaid);
    EXPECT_EQ(a.w.charges, 1);
    EXPECT_EQ(a.w.left, 1);
    EXPECT_EQ(a.w.capacity, 2);
    EXPECT_NEAR(a.w.refill_s, 0.687f, 1e-4f);
    Until(r, a.w.open_t + 0.36);
    const Swung b = Swing(r, 2);
    EXPECT_FALSE(b.w.unpaid);
    EXPECT_EQ(b.w.left, 0);
    Until(r, a.w.open_t + 0.60);
    const Swung c = Swing(r, 3); // commits 0.64 s after the first: its charge is back at 0.687
    EXPECT_TRUE(c.opened);       // the window still opens (the whoosh and trail) ...
    EXPECT_TRUE(c.w.unpaid);        // ... but nothing is paid: no attack
    EXPECT_TRUE(c.empty);        // the dull buzz at its commit
    EXPECT_FALSE(c.paid_late);
    EXPECT_FALSE(c.w.aborted);
    EXPECT_EQ(c.w.charges, 0);
    EXPECT_GT(c.w.next_s, 0.0f);
    EXPECT_LT(c.w.next_s, 0.10f);
    // After the refill it registers.
    Until(r, a.w.open_t + 0.80);
    const Swung d = Swing(r, 4);
    EXPECT_FALSE(d.w.unpaid);
    EXPECT_FALSE(d.empty);
    // The Saw Cleaver (0.487 s) flailed at the swing gate's fastest (0.36 s): the first charge is
    // back for the third stroke, the second one for the fourth (0.97 s), the fifth gets the
    // third's at its commit (1.46 s), the sixth has none (the fourth's is back at 1.95 s). (With
    // -50 ms, 0.517 s: the fifth had none and the sixth the third's.)
    Rig s;
    Weapon(s, 700);
    double t0 = 0.0;
    std::string registered; // R registered at the start, L at the commit, - not at all
    for (unsigned i = 1; i <= 6; ++i) {
        const Swung x = Swing(s, i);
        if (i == 1) {
            t0 = x.w.open_t;
        }
        registered += x.w.unpaid ? '-' : x.w.late ? 'L' : 'R';
        Until(s, t0 + 0.36 * i);
    }
    ASSERT_EQ(registered.size(), 6u);
    EXPECT_EQ(registered.substr(0, 4), std::string{"RRRR"}) << registered;
    EXPECT_EQ(registered[4], 'L') << registered;
    EXPECT_EQ(registered[5], '-') << registered;
}

// The reviewer's case (2026-10-05): two quick slashes, then a game menu (or the virtual screen,
// focus loss, the whip's R1 / R2 route) with no Step for 3 s: both charges are back after it, and
// the next swing registers (the presenter adds the gap with Advance).
TEST(MeleeBudget, ChargesComeBackWhileNoSwingIsStepped) {
    Rig r;
    Weapon(r, 700);
    const Swung a = Swing(r, 1);
    const Swung b = Swing(r, 2);
    EXPECT_FALSE(a.w.unpaid);
    EXPECT_FALSE(b.w.unpaid);
    EXPECT_EQ(r.tracker.BudgetFree(), 0);
    const double before = r.tracker.BudgetClock();
    r.tracker.Advance(3.0); // the menu
    EXPECT_NEAR(r.tracker.BudgetClock(), before + 3.0, 1e-9);
    EXPECT_EQ(r.tracker.BudgetFree(), 2);
    const Swung c = Swing(r, 3);
    EXPECT_FALSE(c.w.unpaid);
    EXPECT_FALSE(c.empty);
    EXPECT_EQ(c.w.left, 1);
    // The cadence clock ran too: the swing after the pause is not "right after" the last one.
    EXPECT_GT(c.w.since_counted, 3.0f);
    // Nothing (or a negative gap) changes nothing.
    const double t = r.tracker.BudgetClock();
    r.tracker.Advance(0.0);
    r.tracker.Advance(-1.0);
    EXPECT_EQ(r.tracker.BudgetClock(), t);
}

TEST(MeleeBudget, HeavyCostsTwoChargedAllAndAWoundUpSwingWithOneLeftIsLight) {
    Rig r;
    Weapon(r, 700);
    const Swung h = Swing(r, 1, true); // wound up: heavy
    EXPECT_EQ(h.w.kind, M::MoveKind::Heavy);
    EXPECT_EQ(h.w.charges, 2);
    EXPECT_EQ(h.w.left, 0);
    EXPECT_NEAR(h.w.refill_s, 0.787f, 1e-4f);
    Until(r, h.w.open_t + 0.45);
    const Swung l = Swing(r, 2); // a light 0.45 s later: nothing free
    EXPECT_TRUE(l.w.unpaid);
    EXPECT_TRUE(l.empty);
    Until(r, h.w.open_t + 0.86);
    EXPECT_EQ(r.tracker.BudgetFree(), 2);
    // A light, then a wound-up stroke with one charge left: a light swing.
    const Swung a = Swing(r, 3);
    EXPECT_EQ(a.w.left, 1);
    Until(r, a.w.open_t + 0.36);
    const Swung b = Swing(r, 4, true);
    EXPECT_FALSE(b.w.unpaid);
    EXPECT_TRUE(b.w.downgraded);
    EXPECT_EQ(b.w.kind, M::MoveKind::Light);
    EXPECT_TRUE(b.w.verdict_known);
    EXPECT_EQ(b.w.charges, 1);
    // The trigger: a charged swing needs them all; with one free it does not register and the
    // trigger's charge stays armed for the next swing.
    Rig c;
    Weapon(c, 700);
    const Swung first = Swing(c, 1);
    EXPECT_EQ(first.w.left, 1);
    c.in.trigger = true;
    for (int i = 0; i < 95; ++i) { // past 1.0 s: fully charged
        c.Step();
    }
    c.in.trigger = false;
    c.Step();
    ASSERT_EQ(c.tracker.ChargeState(), M::Charge::Full);
    EXPECT_EQ(c.tracker.BudgetFree(), 2); // 1.2 s later: all back
    const Swung full = Swing(c, 2);
    EXPECT_EQ(full.w.kind, M::MoveKind::Charged);
    EXPECT_TRUE(full.w.armed);
    EXPECT_EQ(full.w.charges, 2);
    EXPECT_NEAR(full.w.refill_s, 0.853f, 1e-4f);
    // Armed again with nothing free: unpaid, the charge kept.
    c.in.trigger = true;
    for (int i = 0; i < 40; ++i) { // 0.44 s: heavy
        c.Step();
    }
    c.in.trigger = false;
    c.Step();
    ASSERT_EQ(c.tracker.ChargeState(), M::Charge::Heavy);
    const Swung unpaid = Swing(c, 3);
    EXPECT_EQ(unpaid.w.kind, M::MoveKind::Heavy);
    EXPECT_TRUE(unpaid.w.unpaid);
    EXPECT_FALSE(unpaid.w.downgraded); // trigger-armed: never made light
    EXPECT_EQ(c.tracker.ChargeState(), M::Charge::Heavy);
}

TEST(MeleeBudget, AHeavyWithoutAHeavyRowCostsALight) {
    // Rakuyo's form B has no heavy row of the right hand's blade (its R2 is the left blade's): a
    // wound-up or trigger-armed swing opens the light row, and pays a light's charge.
    Rig r;
    Weapon(r, 2700, true);
    EXPECT_EQ(r.in.top, M::MoveKind::Light);
    const Swung h = Swing(r, 1, true); // wound up
    EXPECT_FALSE(h.w.unpaid);
    EXPECT_EQ(h.w.charges, 1);
    EXPECT_EQ(h.w.left, 1);
    EXPECT_NEAR(h.w.refill_s, 0.553f, 1e-4f);
    // The second charge still pays for a quick light right after it.
    Until(r, h.w.open_t + 0.36);
    const Swung l = Swing(r, 2);
    EXPECT_FALSE(l.w.unpaid);
    EXPECT_EQ(l.w.left, 0);
    // The Saw Cleaver's form B has its heavy: two charges, as before.
    Rig s;
    Weapon(s, 700, true);
    EXPECT_EQ(s.in.top, M::MoveKind::Charged);
    const Swung sh = Swing(s, 1, true);
    EXPECT_EQ(sh.w.charges, 2);
}

TEST(MeleeBudget, NotASwingGivesItBackAndAHeavyJudgedLightHoldsOne) {
    Rig r;
    Weapon(r, 700);
    r.Fast(1);
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().charges, 1);
    EXPECT_EQ(r.tracker.BudgetFree(), 1);
    r.in.gate = M::Kind::Ignored; // a wind-up: aborted
    const M::TrackerOutput& o = r.Step();
    EXPECT_TRUE(o.aborted);
    EXPECT_TRUE(o.refunded);
    EXPECT_EQ(r.tracker.BudgetFree(), 2);
    // An onset window whose blade hit something counts (and keeps its charge).
    r.Fast(2);
    r.Step();
    r.in.hit_serial = r.W().serial;
    r.Step();
    r.in.gate = M::Kind::Ignored;
    r.Step();
    EXPECT_TRUE(r.W().committed);
    EXPECT_EQ(r.tracker.BudgetFree(), 1);
    // A wound-up stroke judged light: one charge, back after the light's time.
    Rig h;
    Weapon(h, 700);
    h.Fast(1, true);
    h.Step();
    ASSERT_EQ(h.W().kind, M::MoveKind::Heavy);
    EXPECT_EQ(h.tracker.BudgetFree(), 0);
    h.in.fired = true;
    h.in.fired_stroke = 1;
    h.Step();
    h.in.kind = M::Kind::Light;
    h.in.kind_stroke = 1;
    const M::TrackerOutput& v = h.Step();
    EXPECT_TRUE(v.verdict);
    EXPECT_TRUE(v.refunded);
    EXPECT_EQ(h.W().charges, 1);
    EXPECT_EQ(h.tracker.BudgetFree(), 1);
    EXPECT_NEAR(h.W().refill_s, 0.487f, 1e-4f);
    // The game refused a window: RefundWindow gives its charges back (once).
    const std::uint32_t serial = h.W().serial;
    EXPECT_TRUE(h.tracker.RefundWindow(serial));
    EXPECT_FALSE(h.tracker.RefundWindow(serial));
    EXPECT_EQ(h.tracker.BudgetFree(), 2);
}

TEST(MeleeBudget, AChargeBackBeforeTheCommitPaysForTheSwing) {
    Rig r;
    Weapon(r, 500); // 0.687 s
    const Swung a = Swing(r, 1);
    Until(r, a.w.open_t + 0.36);
    Swing(r, 2);
    // The stroke starts 27 ms before the first charge is back; it commits 55 ms later.
    Until(r, a.w.open_t + 0.66);
    const Swung c = Swing(r, 3, false, 5);
    EXPECT_FALSE(c.w.unpaid);
    EXPECT_TRUE(c.w.late);
    EXPECT_TRUE(c.paid_late);
    EXPECT_FALSE(c.empty);
    EXPECT_EQ(c.w.charges, 1);
}

TEST(MeleeBudget, TransformFlicksNeverPayAndNeverBuzz) {
    Rig r;
    Weapon(r, 500);
    // The timing below is the -50 ms refill's (0.717 s; the second swing's tail runs to ~0.69 s,
    // past the default -80 ms refill's 0.687 s).
    r.cfg.budget.offset_s = -0.050f;
    const Swung a = Swing(r, 1);
    Until(r, a.w.open_t + 0.36);
    Swing(r, 2);
    Until(r, a.w.open_t + 0.70);
    ASSERT_EQ(r.tracker.BudgetFree(), 0);
    // An unpaid stroke the player flicks into the transform: never paid at its commit (a charge is
    // back by then), no empty buzz; the game's flick opens its transform attack.
    r.Fast(3);
    r.Step();
    ASSERT_TRUE(r.W().open);
    ASSERT_TRUE(r.W().unpaid);
    r.Step();
    r.tracker.NoteFlick();
    for (int i = 0; i < 4; ++i) { // 0.744 .. 0.755 s
        r.Step();
    }
    r.in.fired = true;
    r.in.fired_stroke = 3;
    const M::TrackerOutput& o = r.Step();
    EXPECT_TRUE(o.committed);
    EXPECT_FALSE(o.empty);
    EXPECT_FALSE(o.paid_late);
    EXPECT_TRUE(r.W().flick);
    EXPECT_TRUE(r.W().unpaid);
    EXPECT_EQ(r.tracker.BudgetFree(), 1);
    r.Slow();
    for (int i = 0; i < 20; ++i) {
        r.Step();
    }
    // A stroke that starts just after a flick squeeze is the transform's: free.
    const int before = r.tracker.BudgetFree();
    r.tracker.NoteFlick();
    r.Step();
    const Swung f = Swing(r, 4);
    EXPECT_TRUE(f.w.flick);
    EXPECT_FALSE(f.w.unpaid);
    EXPECT_EQ(f.w.charges, 0);
    EXPECT_EQ(r.tracker.BudgetFree(), before);
    // Later strokes pay again.
    Until(r, r.tracker.BudgetClock() + 0.5);
    const Swung g = Swing(r, 5);
    EXPECT_FALSE(g.w.flick);
    EXPECT_EQ(g.w.charges, 1);
    // A squeeze during an open window makes that one the transform's, not the next one too (the
    // game joins one window per transform).
    Until(r, r.tracker.BudgetClock() + 1.0);
    r.Fast(6);
    r.Step();
    ASSERT_TRUE(r.W().open);
    EXPECT_EQ(r.W().charges, 1);
    r.tracker.NoteFlick();
    EXPECT_TRUE(r.W().flick);
    r.Slow();
    for (int i = 0; i < 20; ++i) {
        r.Step();
    }
    const Swung h = Swing(r, 7);
    EXPECT_FALSE(h.w.flick);
    EXPECT_EQ(h.w.charges, 1);
}

TEST(MeleeBudget, OffIsTheOldRule) {
    Rig r;
    r.cfg.budget.enabled = false;
    Weapon(r, 700);
    for (unsigned i = 1; i <= 6; ++i) {
        const Swung s = Swing(r, i);
        EXPECT_FALSE(s.w.unpaid);
        EXPECT_FALSE(s.empty);
        EXPECT_EQ(s.w.charges, 0);
        EXPECT_EQ(s.w.capacity, 0);
    }
    // The cadence damage factor only with the budget off (and for a flick's attack).
    M::DamageConfig c{};
    EXPECT_NEAR(M::DamageFactor(15.0f, 0.3f, M::MoveKind::Light, c), 0.5789f, 1e-3f);
    c.cadence = false;
    EXPECT_NEAR(M::DamageFactor(15.0f, 0.3f, M::MoveKind::Light, c), 1.10f, 1e-4f);
    // With it on, a stroke 0.1 s after a counted swing may open (the budget sets the rate).
    Rig b;
    Weapon(b, 700);
    b.Fast(1);
    b.Step();
    b.in.fired = true;
    b.in.fired_stroke = 1;
    b.Step();
    b.Slow();
    for (int i = 0; i < 6; ++i) {
        b.Step();
    }
    ASSERT_FALSE(b.W().open);
    b.Fast(2);
    b.Step();
    EXPECT_TRUE(b.W().open);
    EXPECT_EQ(b.W().serial, 2u);
}

// melee_stamina_pct: 80 (the default) = 80% of the game's cost, 0 = free
// physical swings, 100 = the game's cost.
TEST(MeleeStamina, EightyPercentByDefault) {
    const M::MeleeSettings d{};
    EXPECT_EQ(d.stamina_pct, 80);
    const M::SwingStamina swing = M::SwingStaminaOf(d.stamina_pct, d.regen_pause);
    EXPECT_EQ(swing.pct, 80);
    EXPECT_TRUE(swing.charge);       // the game takes its cost (the flag is not held) ...
    EXPECT_TRUE(swing.refuse_empty); // ... a swing at 0 or below is refused as the game's own ...
    EXPECT_FALSE(swing.regen_pause); // ... and no regen pause (melee_regen_pause=1 is the default)
    EXPECT_TRUE(d.regen_pause);
    EXPECT_FALSE(M::StaminaRefuses(swing, 91));
    EXPECT_FALSE(M::StaminaRefuses(swing, 1));
    EXPECT_TRUE(M::StaminaRefuses(swing, 0));
    EXPECT_TRUE(M::StaminaRefuses(swing, -3));
}

TEST(MeleeStamina, FreeAtZero) {
    const M::SwingStamina free = M::SwingStaminaOf(0, true);
    EXPECT_EQ(free.pct, 0);
    EXPECT_FALSE(free.charge);
    EXPECT_FALSE(free.refuse_empty);
    EXPECT_FALSE(free.regen_pause); // melee_regen_pause=1 only pauses the game's full cost
    // Never refused for stamina, even out of it (the game lets it go down to -50).
    EXPECT_FALSE(M::StaminaRefuses(free, 91));
    EXPECT_FALSE(M::StaminaRefuses(free, 0));
    EXPECT_FALSE(M::StaminaRefuses(free, -50));
    EXPECT_EQ(M::SwingStaminaOf(-5, true).pct, 0); // clamped
}

TEST(MeleeStamina, CostIsTheGamesAtHundred) {
    const M::SwingStamina cost = M::SwingStaminaOf(100, true);
    EXPECT_EQ(cost.pct, 100);
    EXPECT_TRUE(cost.charge);
    EXPECT_TRUE(cost.refuse_empty);
    EXPECT_TRUE(cost.regen_pause);
    EXPECT_FALSE(M::StaminaRefuses(cost, 1));
    EXPECT_TRUE(M::StaminaRefuses(cost, 0));
    EXPECT_TRUE(M::StaminaRefuses(cost, -12));
    EXPECT_FALSE(M::SwingStaminaOf(100, false).regen_pause);
    EXPECT_TRUE(M::SwingStaminaOf(100, false).charge);
    EXPECT_EQ(M::SwingStaminaOf(250, true).pct, 100); // clamped
    // Only the game's full cost pauses the regen.
    EXPECT_FALSE(M::SwingStaminaOf(99, true).regen_pause);
    EXPECT_FALSE(M::SwingStaminaOf(50, true).regen_pause);
}

TEST(MeleeStamina, PartialCost) {
    EXPECT_EQ(M::PartialCost(30, 10), 3);
    EXPECT_EQ(M::PartialCost(20, 10), 2); // the Saw Cleaver's light
    EXPECT_EQ(M::PartialCost(35, 10), 4); // 3.5 rounds up
    EXPECT_EQ(M::PartialCost(34, 10), 3);
    EXPECT_EQ(M::PartialCost(7, 10), 1);  // 0.7: at least 1 of a cost above 0
    EXPECT_EQ(M::PartialCost(4, 10), 1);  // 0.4: still 1
    EXPECT_EQ(M::PartialCost(1, 10), 1);
    EXPECT_EQ(M::PartialCost(0, 10), 0);  // a row without a cost stays free
    EXPECT_EQ(M::PartialCost(-5, 10), 0);
    EXPECT_EQ(M::PartialCost(30, 0), 0);  // free
    EXPECT_EQ(M::PartialCost(30, 100), 30);
    EXPECT_EQ(M::PartialCost(30, 50), 15);
    EXPECT_EQ(M::PartialCost(50, 25), 13); // 12.5 rounds up
}

TEST(MeleeStamina, PartialSwingGivesBackAllButTheShare) {
    // A light (row 30) at a full gauge: the game takes 30, 27 go back in the same call.
    M::SwingPaid p = M::PartialSwing(91, 61, 10, 30);
    EXPECT_EQ(p.stamina, 88);
    EXPECT_EQ(p.full, 30);
    EXPECT_EQ(p.paid, 3);
    // A small row: at least 1.
    p = M::PartialSwing(91, 84, 10, 7);
    EXPECT_EQ(p.stamina, 90);
    EXPECT_EQ(p.paid, 1);
    // Nothing taken (a row without a cost, or the attack existed already): left as it is.
    p = M::PartialSwing(91, 91, 10, 0);
    EXPECT_EQ(p.stamina, 91);
    EXPECT_EQ(p.full, 0);
    EXPECT_EQ(p.paid, 0);
    EXPECT_EQ(M::PartialSwing(60, 64, 10, 30).stamina, 64); // the stamina rose meanwhile
    // The measured drop is the game's cost (not the row's) while the floor did not clip it.
    p = M::PartialSwing(91, 51, 10, 30);
    EXPECT_EQ(p.full, 40);
    EXPECT_EQ(p.stamina, 87);
    // Low stamina, the game's drop clipped at -50: the row's cost is the full one.
    p = M::PartialSwing(1, -50, 10, 80); // the game: 1 - 80 -> -50 (51 taken)
    EXPECT_EQ(p.full, 80);
    EXPECT_EQ(p.paid, 8);
    EXPECT_EQ(p.stamina, -7);
    p = M::PartialSwing(1, -29, 10, 30); // not clipped
    EXPECT_EQ(p.stamina, -2);
    // Never below the floor.
    p = M::PartialSwing(-49, -50, 10, 30);
    EXPECT_EQ(p.stamina, -50);
    EXPECT_EQ(p.paid, 1);
}

TEST(MeleeStamina, PartialSwingAtZeroAndHundredMatchesTheOldPaths) {
    // pct 0 = the free swing (FreeSwingStamina: the value before), pct 100 = the game's own drop.
    const int before[] = {91, 5, 0, -30, 60, 91};
    const int after[] = {71, -15, -20, -50, 64, 91};
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(M::PartialSwing(before[i], after[i], 0, 20).stamina,
                  M::FreeSwingStamina(before[i], after[i]));
        EXPECT_EQ(M::PartialSwing(before[i], after[i], 100, 20).stamina, after[i]);
    }
    EXPECT_EQ(M::PartialSwing(91, 71, 100, 20).paid, 20);
    EXPECT_EQ(M::PartialSwing(91, 71, 0, 20).paid, 0);
}

TEST(MeleeStamina, TenthOfALongChain) {
    // Twenty lights (row 20) back to back from a full 91 with no regen at all: 2 each, 51 left
    // (the game's own cost would have refused the sixth).
    int s = 91;
    const M::SwingStamina rule = M::SwingStaminaOf(10, true);
    int swings = 0;
    for (int i = 0; i < 20; ++i) {
        if (M::StaminaRefuses(rule, s)) {
            break;
        }
        s = M::PartialSwing(s, std::max(s - 20, M::StaminaFloor), rule.pct, 20).stamina;
        ++swings;
    }
    EXPECT_EQ(swings, 20);
    EXPECT_EQ(s, 51);
}

TEST(MeleeStamina, FlagHeldForTheCallOnly) {
    // The no-stamina-consumption bit (ChrData +0x200, 0x80) set for the call ...
    EXPECT_EQ(M::FreeSwingFlags(0x00), 0x80);
    EXPECT_EQ(M::FreeSwingFlags(0x41), 0xC1);
    EXPECT_EQ(M::FreeSwingFlags(0x80), 0x80);
    // ... and as it was after it; the other bits as the call left them.
    EXPECT_EQ(M::RestoredFlags(0x80, 0x00), 0x00);
    EXPECT_EQ(M::RestoredFlags(0xC1, 0x41), 0x41);
    EXPECT_EQ(M::RestoredFlags(0x83, 0x01), 0x03);
    EXPECT_EQ(M::RestoredFlags(0x80, 0x80), 0x80); // the game had it set: it stays set
    EXPECT_EQ(M::RestoredFlags(M::FreeSwingFlags(0x25), 0x25), 0x25);
}

TEST(MeleeStamina, FreeSwingNeverDips) {
    // A cost that still came off (the flag not matched): the value before goes back.
    EXPECT_EQ(M::FreeSwingStamina(91, 71), 91);
    EXPECT_EQ(M::FreeSwingStamina(5, -15), 5);
    EXPECT_EQ(M::FreeSwingStamina(0, -20), 0);
    EXPECT_EQ(M::FreeSwingStamina(-30, -50), -30);
    // Nothing taken (the flag held), or the stamina rose meanwhile: left as it is.
    EXPECT_EQ(M::FreeSwingStamina(91, 91), 91);
    EXPECT_EQ(M::FreeSwingStamina(60, 64), 64);
}

} // namespace

namespace {
TEST(MeleeStamina, StrongAttacksAlwaysPayFullCost) {
    for (int pct : {0, 10, 50, 100}) {
        for (auto kind : {M::MoveKind::Heavy, M::MoveKind::Charged}) {
            const auto rule = M::AttackStaminaOf(kind, pct, true);
            EXPECT_EQ(rule.pct, 100);
            EXPECT_TRUE(rule.charge);
            EXPECT_TRUE(rule.refuse_empty);
            EXPECT_TRUE(rule.regen_pause);
            EXPECT_EQ(M::PartialSwing(100, 60, rule.pct, 40).paid, 40);
        }
        EXPECT_EQ(M::AttackStaminaOf(M::MoveKind::Light, pct, true).pct, pct);
    }
}
TEST(MeleeStamina, LowStaminaUsesLightRowAndModifiersForBothForms) {
    for (const auto& weapon : M::Moves) {
        for (bool form_b : {false, true}) {
            auto light = M::ChooseMove(weapon, form_b, M::MoveKind::Light);
            for (auto kind : {M::MoveKind::Heavy, M::MoveKind::Charged}) {
                auto strong = M::ChooseMove(weapon, form_b, kind);
                if (!strong.row || strong.kind == M::MoveKind::Light) continue;
                const int cost = strong.row->stamina;
                ASSERT_GT(cost, 0);
                auto enough = M::AffordableMove(strong, light.row, cost);
                EXPECT_EQ(enough.row, strong.row);
                EXPECT_EQ(enough.kind, strong.kind);
                auto fallback = M::AffordableMove(strong, light.row, cost - 1);
                ASSERT_EQ(fallback.row, light.row);
                EXPECT_EQ(fallback.kind, M::MoveKind::Light);
                EXPECT_TRUE(fallback.lighter);
                if (!fallback.row) continue;
                for (int pct : {10, 50, 100}) {
                    auto rule = M::AttackStaminaOf(fallback.kind, pct, true);
                    EXPECT_EQ(M::PartialSwing(100, 100 - fallback.row->stamina,
                                             rule.pct, fallback.row->stamina).paid,
                              M::PartialCost(light.row->stamina, pct));
                    M::DamageConfig cfg;
                    EXPECT_FLOAT_EQ(M::DamageFactor(20.f, 10.f, fallback.kind, cfg),
                                    M::DamageFactor(20.f, 10.f, M::MoveKind::Light, cfg));
                    EXPECT_TRUE(M::StaminaRefuses(rule, 0));
                }
            }
        }
    }
}
TEST(MeleeStamina, MissingLightRowDoesNotKeepUnaffordableStrongAttack) {
    const auto heavy = M::ChooseMove(Moves(700), false, M::MoveKind::Heavy);
    const auto fallback = M::AffordableMove(heavy, nullptr, 1);
    EXPECT_EQ(fallback.row, nullptr);
    EXPECT_EQ(fallback.kind, M::MoveKind::Light);
}
} // namespace

namespace {
TEST(MeleeStamina, DowngradedStrongSwingUsesLightComboBudgetAndRecovery) {
    Rig r;
    Weapon(r, 700);
    r.Fast(1, true);
    ASSERT_TRUE(r.Step().opened);
    ASSERT_EQ(r.W().kind, M::MoveKind::Heavy);
    ASSERT_EQ(r.W().charges, 2);
    const auto serial = r.W().serial;
    EXPECT_FALSE(r.tracker.ResolveLightAttack(serial + 1, r.cfg.budget));
    EXPECT_TRUE(r.tracker.ResolveLightAttack(serial, r.cfg.budget));
    EXPECT_EQ(r.W().kind, M::MoveKind::Light);
    EXPECT_EQ(r.W().verdict, M::MoveKind::Light);
    EXPECT_TRUE(r.W().verdict_known);
    EXPECT_EQ(r.W().charges, 1);
    EXPECT_EQ(r.tracker.BudgetFree(), 1);
    EXPECT_NEAR(r.W().refill_s, M::BudgetRefill(r.in.recover[0], r.cfg.budget), 1e-5f);
    EXPECT_FALSE(r.tracker.ResolveLightAttack(serial, r.cfg.budget));
    EXPECT_EQ(r.tracker.BudgetFree(), 1);
}
} // namespace

namespace {
// melee_left: the left hand pays from the right hand's budget, on its clock, so a left swing takes
// the weapon's next swing slot (the Saw Cleaver: two charges, 0.487 s each).
TEST(MeleeBudget, BothHandsPayFromOnePool) {
    Rig right;
    Rig left;
    Weapon(right, 700);
    Weapon(left, 700);
    left.tracker.PayFrom(&right.tracker);
    const Swung a = Swing(right, 1);
    const Swung b = Swing(left, 1); // the left hand's own serial 1
    EXPECT_FALSE(a.w.unpaid);
    EXPECT_FALSE(b.w.unpaid);
    EXPECT_EQ(b.w.left, 0);
    EXPECT_EQ(right.tracker.BudgetFree(), 0);
    EXPECT_EQ(left.tracker.BudgetFree(), 0);
    EXPECT_EQ(left.tracker.BudgetClock(), right.tracker.BudgetClock());
    // Nothing free for either hand until the right hand's clock brings a charge back.
    const Swung c = Swing(left, 2);
    EXPECT_TRUE(c.w.unpaid);
    EXPECT_TRUE(c.empty);
    Until(right, a.w.open_t + 0.50);
    EXPECT_EQ(left.tracker.BudgetFree(), 1);
    const Swung d = Swing(left, 3);
    EXPECT_FALSE(d.w.unpaid);
    EXPECT_EQ(right.tracker.BudgetFree(), 0);
    // Each hand's refund gives back its own window's charges only (the serials are each hand's).
    Rig r2;
    Rig l2;
    Weapon(r2, 700);
    Weapon(l2, 700);
    l2.tracker.PayFrom(&r2.tracker);
    Swing(r2, 1);
    Swing(l2, 1);
    ASSERT_EQ(r2.tracker.BudgetFree(), 0);
    EXPECT_TRUE(l2.tracker.RefundWindow(1));
    EXPECT_EQ(r2.tracker.BudgetFree(), 1);
    EXPECT_FALSE(l2.tracker.RefundWindow(1)); // once
    EXPECT_TRUE(r2.tracker.RefundWindow(1));
    EXPECT_EQ(r2.tracker.BudgetFree(), 2);
}

// melee_left: a left window opened with a right swing (the off hand's counter-motion) is dropped:
// aborted, committed or not, its charge back; one that hit something stays a swing.
TEST(MeleeWindow, ADroppedWindowIsAbortedAndGivesItsChargeBackUnlessItHit) {
    Rig r;
    Weapon(r, 700);
    r.Fast(1);
    r.Step();
    r.in.fired = true;
    r.in.fired_stroke = 1;
    r.Step();
    ASSERT_TRUE(r.W().open);
    ASSERT_TRUE(r.W().committed);
    ASSERT_EQ(r.tracker.BudgetFree(), 1);
    const M::TrackerOutput o = r.tracker.Drop("counter-motion");
    EXPECT_TRUE(o.closed);
    EXPECT_TRUE(o.aborted);
    EXPECT_TRUE(o.refunded);
    EXPECT_FALSE(r.W().open);
    EXPECT_TRUE(r.W().aborted);
    EXPECT_EQ(r.W().charges, 0);
    EXPECT_EQ(r.tracker.BudgetFree(), 2);
    EXPECT_STREQ(r.W().why, "counter-motion");
    EXPECT_FALSE(r.tracker.Drop("again").closed); // nothing open: nothing
    r.Slow();
    r.Step();
    r.Fast(2);
    r.Step();
    ASSERT_TRUE(r.W().open);
    r.in.hit_serial = r.W().serial;
    r.Step();
    ASSERT_TRUE(r.W().hit);
    EXPECT_FALSE(r.tracker.Drop("counter-motion").closed);
    EXPECT_TRUE(r.W().open);
    EXPECT_EQ(r.W().charges, 1);
}
} // namespace

namespace {
// melee_left: a window the presenter dropped (closed and aborted) before the game ever saw it open
// is stale to the game's latch (bloodborne_melee.cpp LeftMeleeTick): no attack, no stamina.
TEST(MeleeLatch, AWindowFirstSeenClosedAndAbortedMakesNoAttack) {
    M::LatchState s{};
    M::LatchInput in{};
    in.have = true;
    in.serial = 7;
    in.open = false;
    in.stale = true;
    in.gates = true;
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    Expect(s, in, M::LatchStep::Idle, __LINE__);
    // The next window, open: an attack as before.
    in.serial = 8;
    in.open = true;
    in.stale = false;
    Expect(s, in, M::LatchStep::Create, __LINE__);
}
} // namespace

namespace {
// The reviewer's case (2026-10-12): one charge left, the off hand's counter-motion took it (its
// window opened first), the right swing committed unpaid; the left one dropped, its charge back
// pays the right swing at once (PayLate), so the attack the player meant still goes out.
TEST(MeleeBudget, ADroppedCounterMotionPaysTheRightSwingItStarved) {
    Rig right;
    Rig left;
    Weapon(right, 700);
    Weapon(left, 700);
    left.tracker.PayFrom(&right.tracker);
    Swing(right, 1); // one of the two charges used
    ASSERT_EQ(right.tracker.BudgetFree(), 1);
    left.Fast(1);
    left.Step(); // the left window opens first and takes the last charge
    ASSERT_TRUE(left.W().open);
    ASSERT_EQ(left.W().charges, 1);
    right.Fast(2);
    right.Step();
    right.in.fired = true;
    right.in.fired_stroke = 2;
    EXPECT_TRUE(right.Step().empty); // committed with nothing free
    ASSERT_TRUE(right.W().unpaid);
    EXPECT_FALSE(right.tracker.PayLate(right.cfg)); // still nothing free
    left.tracker.Drop("the off hand's counter-motion");
    EXPECT_EQ(right.tracker.BudgetFree(), 1);
    EXPECT_TRUE(right.tracker.PayLate(right.cfg));
    EXPECT_FALSE(right.W().unpaid);
    EXPECT_TRUE(right.W().late);
    EXPECT_EQ(right.W().charges, 1);
    EXPECT_EQ(right.tracker.BudgetFree(), 0);
    EXPECT_FALSE(right.tracker.PayLate(right.cfg)); // paid once ...
    EXPECT_FALSE(right.W().unpaid);                  // ... and still paid
    EXPECT_EQ(right.W().charges, 1);
}
} // namespace

namespace {
// The reviewer's second case (2026-10-12): both charges free, the off hand's counter-motion took
// one; a wound-up right swing waits for it (hold_ago) instead of settling for a light, and once
// the left one is dropped it is paid as the heavy it is.
TEST(MeleeBudget, AWoundUpSwingWaitsForTheCounterMotionsCharge) {
    Rig right;
    Rig left;
    Weapon(right, 700);
    Weapon(left, 700);
    left.tracker.PayFrom(&right.tracker);
    left.Fast(1);
    left.Step();
    ASSERT_EQ(left.W().charges, 1);
    right.in.hold_ago = 0.0f; // the left one just started
    right.Fast(1, true);      // wound up: a heavy
    right.Step();
    EXPECT_TRUE(right.W().unpaid); // not downgraded: it waits
    EXPECT_EQ(right.W().kind, M::MoveKind::Heavy);
    right.in.fired = true;
    right.in.fired_stroke = 1;
    right.Step();
    EXPECT_TRUE(right.W().unpaid);
    left.tracker.Drop("the off hand's counter-motion");
    EXPECT_TRUE(right.tracker.PayLate(right.cfg));
    EXPECT_EQ(right.W().kind, M::MoveKind::Heavy);
    EXPECT_EQ(right.W().charges, 2);
    EXPECT_FALSE(right.W().downgraded);
    // Only one charge back once the left one is dropped (the other used before): a light then,
    // never left waiting.
    Rig r3;
    Rig l3;
    Weapon(r3, 700);
    Weapon(l3, 700);
    l3.tracker.PayFrom(&r3.tracker);
    Swing(r3, 1);
    l3.Fast(1);
    l3.Step();
    ASSERT_EQ(r3.tracker.BudgetFree(), 0);
    r3.in.hold_ago = 0.0f;
    r3.Fast(2, true);
    r3.Step();
    ASSERT_TRUE(r3.W().unpaid);
    l3.tracker.Drop("the off hand's counter-motion");
    EXPECT_TRUE(r3.tracker.PayLate(r3.cfg));
    EXPECT_EQ(r3.W().kind, M::MoveKind::Light);
    EXPECT_TRUE(r3.W().downgraded);
    EXPECT_EQ(r3.W().charges, 1);
    // Without the hold (no off-hand stroke), one charge short: a light at once, as before.
    Rig r2;
    Rig l2;
    Weapon(r2, 700);
    Weapon(l2, 700);
    l2.tracker.PayFrom(&r2.tracker);
    l2.Fast(1);
    l2.Step();
    r2.Fast(1, true);
    r2.Step();
    EXPECT_FALSE(r2.W().unpaid);
    EXPECT_TRUE(r2.W().downgraded);
    EXPECT_EQ(r2.W().kind, M::MoveKind::Light);
}
} // namespace

namespace {
// The reviewer's case (2026-10-12): a wound-up right swing waiting for the counter-motion's charge
// was judged light at its commit; paid late it takes a light's charge (and is the light it is),
// never a heavy's two.
TEST(MeleeBudget, APaymentLateForASwingJudgedLightTakesALightsCharge) {
    Rig right;
    Rig left;
    Weapon(right, 700);
    Weapon(left, 700);
    left.tracker.PayFrom(&right.tracker);
    left.Fast(1);
    left.Step();
    right.in.hold_ago = 0.0f;
    right.Fast(1, true);
    right.Step();
    ASSERT_TRUE(right.W().unpaid);
    right.in.fired = true;
    right.in.fired_stroke = 1;
    right.in.kind = M::Kind::Light; // the classifier: it stopped short of the big arc
    right.in.kind_stroke = 1;
    right.Step();
    ASSERT_TRUE(right.W().verdict_known);
    ASSERT_EQ(right.W().verdict, M::MoveKind::Light);
    left.tracker.Drop("the off hand's counter-motion");
    EXPECT_TRUE(right.tracker.PayLate(right.cfg));
    EXPECT_EQ(right.W().kind, M::MoveKind::Light);
    EXPECT_EQ(right.W().charges, 1);
    EXPECT_EQ(right.tracker.BudgetFree(), 1);
}
} // namespace

namespace {
// The reviewer's case (2026-10-12): a new wound-up swing replaces a window still open (the first
// swing's charge already back) while the off hand's counter-motion holds a charge. The hold is
// measured against the new window's own start, so it waits instead of settling for a light, and is
// paid as the heavy it is once the left one is dropped.
TEST(MeleeBudget, AHeavyReplacingAnOpenWindowIsHeldByItsOwnStart) {
    Rig right;
    Rig left;
    Weapon(right, 700);
    Weapon(left, 700);
    left.tracker.PayFrom(&right.tracker);
    right.Fast(1);
    right.Step();
    right.in.fired = true;
    right.in.fired_stroke = 1;
    right.Step();
    ASSERT_TRUE(right.W().committed);
    for (int i = 0; i < 45; ++i) { // 0.5 s, still open: its charge (0.487 s) back meanwhile
        right.Fast(1);
        right.Step();
    }
    ASSERT_TRUE(right.W().open);
    ASSERT_EQ(right.tracker.BudgetFree(), 2);
    const std::uint32_t first = right.W().serial;
    left.Fast(1);
    left.Step(); // the off hand's motion takes one
    ASSERT_EQ(right.tracker.BudgetFree(), 1);
    right.in.hold_ago = 0.0f; // it started just now, as the new swing does
    right.Fast(2, true);
    right.in.fired = true;
    right.in.fired_stroke = 2;
    right.Step(); // the new stroke's commit replaces the open window
    ASSERT_NE(right.W().serial, first);
    EXPECT_TRUE(right.W().unpaid); // held, not settled for a light
    EXPECT_EQ(right.W().kind, M::MoveKind::Heavy);
    left.tracker.Drop("the off hand's counter-motion");
    EXPECT_TRUE(right.tracker.PayLate(right.cfg));
    EXPECT_EQ(right.W().kind, M::MoveKind::Heavy);
    EXPECT_EQ(right.W().charges, 2);
    // Billed again at its commit (an onset window, 0.1 s old by then): the off-hand stroke started
    // 0.1 s before this swing's start is still its counter-motion (0.2 s ago, but 0.1 s from the
    // swing's own start): still held, and paid as the heavy once the left one is dropped.
    Rig r4;
    Rig l4;
    Weapon(r4, 700);
    Weapon(l4, 700);
    l4.tracker.PayFrom(&r4.tracker);
    l4.Fast(1);
    l4.Step();
    r4.in.hold_ago = 0.1f;
    r4.Fast(1, true);
    r4.Step(); // the onset window: held
    ASSERT_TRUE(r4.W().open);
    ASSERT_TRUE(r4.W().unpaid);
    for (int i = 0; i < 9; ++i) { // 0.1 s on
        r4.in.hold_ago += r4.in.dt;
        r4.Fast(1, true);
        r4.Step();
    }
    r4.in.fired = true;
    r4.in.fired_stroke = 1;
    r4.Step(); // its commit: billed again, still held
    ASSERT_TRUE(r4.W().committed);
    EXPECT_TRUE(r4.W().unpaid);
    l4.tracker.Drop("the off hand's counter-motion");
    EXPECT_TRUE(r4.tracker.PayLate(r4.cfg));
    EXPECT_EQ(r4.W().kind, M::MoveKind::Heavy);
    EXPECT_EQ(r4.W().charges, 2);
    // An off-hand stroke that started long before this window (0.4 s): no hold, a light at once.
    Rig r2;
    Rig l2;
    Weapon(r2, 700);
    Weapon(l2, 700);
    l2.tracker.PayFrom(&r2.tracker);
    l2.Fast(1);
    l2.Step();
    r2.in.hold_ago = 0.4f;
    r2.Fast(1, true);
    r2.Step();
    EXPECT_FALSE(r2.W().unpaid);
    EXPECT_TRUE(r2.W().downgraded);
}
} // namespace
