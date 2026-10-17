// SPDX-License-Identifier: GPL-2.0-or-later

// The whip's damage pace (core/vr/whip_damage.h PaceMeter, vr.ini whip_pace): the whip forms
// register sooner than the game's own pace (the presenter's quicker combo-budget refills, every
// spin revolution and wrist crack a window of its own), but a window that hurts before the game could
// have started its move (the move before it had its recovery, core/vr/whip_moves.h WhipRecover)
// loses the share of its own recovery it is early by. Each move of both whips hits in full at the
// game's pace and for half at twice it; the game's R1 -> R2 hits in full; a whiff costs nothing; a
// pause banks one full hit only; the floor of a window on top of the last is paid back, and windows
// closer than that (no budget, more charges) get only the room left on the game's clock; no mix of
// moves, whatever budget lets them through, beats the best move's rate; and a spin or a run of
// cracks on the presenter's refills deals the game's own damage per second.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/melee_window.h"
#include "core/vr/whip_damage.h"
#include "core/vr/whip_moves.h"
#include "core/vr/whip_spin.h"

namespace {

namespace M = VR::Melee;
namespace W = VR::Whip;

std::int64_t Ns(double s) {
    return static_cast<std::int64_t>(std::llround(s * 1e9));
}

// The presenter's combo-budget recovery of a whip form's move (openxr_presenter.cpp, the whip
// refills of 2026-10-15, melee_budget_scale 1): the game's own less 270 ms (light) / 150 ms (heavy,
// charged), the Beast Cutter another 139 / 274 ms, the Threaded Cane 100 / 250 ms.
float PresenterRecover(std::int32_t variation, std::size_t k) {
    const bool light = k == 0;
    const float sooner = (light ? 0.270f : 0.150f) +
                         (variation == 2400   ? (light ? 0.139f : 0.274f)
                          : variation == 2200 ? (light ? 0.100f : 0.250f)
                                              : 0.0f);
    return W::WhipRecover(variation, k) - sooner;
}

TEST(WhipPace, ThePresentersRefillsRegisterSoonerThanTheGamesPace) {
    // The refills the weapons page gives (bloodborne-vr-weapons.md): what the pace has to hold.
    const M::BudgetConfig b{};
    const float cane[3] = {0.317f, 0.853f, 0.787f};
    const float cutter[3] = {0.744f, 1.563f, 1.596f};
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(M::BudgetRefill(PresenterRecover(2200, k), b), cane[k], 0.0015f) << k;
        EXPECT_NEAR(M::BudgetRefill(PresenterRecover(2400, k), b), cutter[k], 0.0015f) << k;
        EXPECT_LT(cane[k], W::WhipRecover(2200, k));
        EXPECT_LT(cutter[k], W::WhipRecover(2400, k));
    }
}

TEST(WhipPace, EachMoveOfBothWhipsHitsInFullAtTheGamesPaceAndHalfAtTwiceIt) {
    for (const W::WhipMoves& whip : W::Whips) {
        for (std::size_t k = 0; k < 3; ++k) {
            const float r = W::WhipRecover(whip.variation, k);
            ASSERT_GT(r, 0.5f);
            W::PaceMeter game, twice;
            double dealt_game = 0.0, dealt_twice = 0.0;
            constexpr int N = 20;
            for (int i = 0; i < N; ++i) {
                const float g = game.Pay(Ns(5.0 + i * static_cast<double>(r)), r);
                EXPECT_NEAR(g, 1.0f, 1e-4f) << whip.name << " move " << k << " lash " << i;
                dealt_game += g;
                const float h = twice.Pay(Ns(5.0 + i * 0.5 * static_cast<double>(r)), r);
                EXPECT_NEAR(h, i == 0 ? 1.0f : 0.5f, 1e-4f)
                    << whip.name << " move " << k << " lash " << i;
                dealt_twice += h;
            }
            // Past the first lash, full hits per second: the game's own (1 / its recovery) both ways.
            const double game_rate = (dealt_game - 1.0) / ((N - 1) * static_cast<double>(r));
            const double twice_rate = (dealt_twice - 1.0) / ((N - 1) * 0.5 * static_cast<double>(r));
            EXPECT_NEAR(game_rate * r, 1.0, 1e-3) << whip.name << " move " << k;
            EXPECT_NEAR(twice_rate * r, 1.0, 1e-3) << whip.name << " move " << k;
        }
    }
}

TEST(WhipPace, AWhiffCostsNothingAndAPauseBanksOneFullHitOnly) {
    const float r = W::WhipRecover(2200, 0);
    W::PaceMeter m;
    EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), r), 1.0f) << "the first lash that hurts";
    // A lash at 0.3 R missed (nothing paid): the next is measured from the last one that hurt.
    EXPECT_NEAR(m.Pay(Ns(1.0 + 0.6 * r), r), 0.6f, 1e-4f);
    // A long pause banks one full hit, not more: the lash on top of it gets the floor ...
    EXPECT_FLOAT_EQ(m.Pay(Ns(30.0), r), 1.0f);
    EXPECT_NEAR(m.Pay(Ns(30.0 + 0.1 * r), r), W::PaceConfig{}.floor, 1e-4f);
    // ... and the next pays its extra back (0.15 for 0.10), then they are full again.
    EXPECT_NEAR(m.Pay(Ns(30.0 + 1.1 * r), r), 0.95f, 1e-4f);
    EXPECT_NEAR(m.Pay(Ns(30.0 + 2.1 * r), r), 1.0f, 1e-4f);
}

TEST(WhipPace, DoubleLashesAFrameApartKeepTheGamesRate) {
    // A pair of windows one frame apart every recovery (the budget's two charges: a stroke and a
    // crack, or two revolutions), both hurting: the second gets the floor and the next pair's
    // first pays it back, so the pairs deal the game's rate, not twice it.
    const float r = W::WhipRecover(2400, 0);
    const W::PaceConfig c{};
    W::PaceMeter m;
    double dealt = 0.0;
    constexpr int Pairs = 30;
    for (int i = 0; i < Pairs; ++i) {
        const double t = i * static_cast<double>(r);
        const float first = m.Pay(Ns(t), r);
        const float second = m.Pay(Ns(t + 1.0 / 90.0), r);
        EXPECT_NEAR(second, c.floor, 1e-4f) << "pair " << i;
        if (i > 0) {
            EXPECT_NEAR(first + second, 1.0f, 1e-3f) << "pair " << i;
        }
        dealt += first + second;
    }
    const double span = (Pairs - 1) * static_cast<double>(r) + 1.0 / 90.0;
    EXPECT_LE(dealt, span / r + 1.0 + c.floor + 1e-3) << "at most one banked hit and one floor owed";
    EXPECT_GE(dealt, span / r);
    std::printf("  %d double lashes: %.2f full hits (the game's pace: %.2f; unpaced: %d)\n", Pairs,
                dealt, span / r + 1.0, 2 * Pairs);
}

TEST(WhipPace, ALateHitOfAnOlderWindowNeverTurnsTheClockBack) {
    const float r = 1.0f;
    W::PaceMeter m;
    EXPECT_FLOAT_EQ(m.Pay(Ns(10.0), r), 1.0f);
    EXPECT_NEAR(m.Pay(Ns(9.5), r), W::PaceConfig{}.floor, 1e-5f) << "no time since: the floor";
    EXPECT_NEAR(m.Pay(Ns(11.0), r), 1.0f - W::PaceConfig{}.floor, 1e-4f)
        << "measured from 10 s, the floor paid back";
}

TEST(WhipPace, TheGamesROneThenRTwoHitsInFullALightRightAfterAHeavyPaysForIt) {
    // The game starts a move once the move before it had its recovery: its R2 one light's recovery
    // after its R1. A light one light's recovery after that heavy comes early by what the heavy's
    // longer recovery still had to go.
    for (const W::WhipMoves& whip : W::Whips) {
        const float rl = W::WhipRecover(whip.variation, 0);
        const float rh = W::WhipRecover(whip.variation, 1);
        const float rc = W::WhipRecover(whip.variation, 2);
        W::PaceMeter m;
        EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), rl), 1.0f) << whip.name;
        EXPECT_NEAR(m.Pay(Ns(1.0 + rl), rh), 1.0f, 1e-4f) << whip.name << ": R1 -> R2";
        const float after_heavy = m.Pay(Ns(1.0 + 2.0 * rl), rl);
        EXPECT_NEAR(after_heavy, 1.0f - (rh - rl) / rl, 1e-4f) << whip.name;
        EXPECT_GT(after_heavy, W::PaceConfig{}.floor) << whip.name;
        // ... the game's own pace after it: in full again; and a charged one after a light.
        EXPECT_NEAR(m.Pay(Ns(1.0 + 3.0 * rl), rc), 1.0f, 1e-4f) << whip.name;
        EXPECT_NEAR(m.Pay(Ns(1.0 + 3.0 * rl + rc), rl), 1.0f, 1e-4f) << whip.name;
        std::printf("  %s: a light one light's recovery after a heavy: x%.2f\n",
                    std::string(whip.name).c_str(), after_heavy);
    }
}

TEST(WhipPace, ALightAtTheEarliestTheBudgetAllowsAfterAHeavyOrCharged) {
    // A heavy or charged lash uses both charges; they come back together at its refill (the
    // presenter's), and a light then is the soonest the default budget lets one through. The game's
    // recovery runs from its move's start, a whip window from its lash, so the long move's wind-up
    // counts against the light: the Threaded Cane's about x0.37, the Beast Cutter's about x0.59,
    // never under the floor.
    const M::BudgetConfig b{};
    for (const W::WhipMoves& whip : W::Whips) {
        const float rl = W::WhipRecover(whip.variation, 0);
        for (const std::size_t k : {std::size_t{1}, std::size_t{2}}) {
            const float rk = W::WhipRecover(whip.variation, k);
            M::ComboBudget budget;
            budget.SetCapacity(b.charges);
            W::PaceMeter m;
            const double t0 = 2.0;
            ASSERT_TRUE(budget.Spend(1, M::BudgetCost(static_cast<M::MoveKind>(k), b.charges),
                                     M::BudgetRefill(PresenterRecover(whip.variation, k), b), t0));
            EXPECT_FLOAT_EQ(m.Pay(Ns(t0), rk), 1.0f);
            double t = t0;
            const float light_refill = M::BudgetRefill(PresenterRecover(whip.variation, 0), b);
            while (!budget.Spend(2, 1, light_refill, t)) {
                t += 1.0 / 90.0;
            }
            const float share = m.Pay(Ns(t), rl);
            EXPECT_NEAR(share, 1.0 - (rk - (t - t0)) / rl, 1e-3) << whip.name << " move " << k;
            EXPECT_GE(share, W::PaceConfig{}.floor) << whip.name << " move " << k;
            std::printf("  %s: a light %.3f s after a %s (its refill): x%.2f\n",
                        std::string(whip.name).c_str(), t - t0, k == 1 ? "heavy" : "charged",
                        share);
        }
    }
}

// 20000 random whip windows (light, heavy or charged, 0 .. `max_gap_s` apart), the ones the combo
// budget `b` pays at the presenter's whip refills (a light one charge, a heavy two, a charged all;
// every one with the budget off) all hurting: the damage dealt (each row's motion value x its
// share) never beats the whip's best move at the game's own pace over the same time (plus the one
// hit a pause banks and the floor owed). `floor_holds`: every window gets at least the floor.
void RandomMixKeepsTheBestMovesRate(const M::BudgetConfig& b, double max_gap_s, bool floor_holds,
                                    const char* what) {
    std::uint32_t seed = 20261015u;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const unsigned gap_ms = static_cast<unsigned>(max_gap_s * 1000.0 + 0.5);
    for (const W::WhipMoves& whip : W::Whips) {
        double best = 0.0, longest = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            const double r = W::WhipRecover(whip.variation, k);
            best = std::max(best, whip.rows[k].mv / r);
            longest = std::max(longest, r);
        }
        M::ComboBudget budget;
        budget.SetCapacity(b.charges);
        W::PaceMeter m;
        double t = 0.0, first = -1.0, dealt = 0.0, unpaced = 0.0;
        int windows = 0, below_floor = 0;
        std::uint32_t serial = 0;
        for (int i = 0; i < 20000; ++i) {
            t += static_cast<double>(next() % (gap_ms + 1)) * 0.001;
            const std::size_t k = next() % 3;
            const int cost = M::BudgetCost(static_cast<M::MoveKind>(k), b.charges);
            const float refill = M::BudgetRefill(PresenterRecover(whip.variation, k), b);
            if (b.enabled && !budget.Spend(++serial, cost, refill, t)) {
                continue; // not registered: no attack
            }
            const float share = m.Pay(Ns(t), W::WhipRecover(whip.variation, k));
            ASSERT_GE(share, 0.0f) << what;
            ASSERT_LE(share, 1.0f) << what;
            if (floor_holds) {
                ASSERT_GE(share, W::PaceConfig{}.floor) << what;
            }
            below_floor += share < W::PaceConfig{}.floor - 1e-6f ? 1 : 0;
            dealt += share * whip.rows[k].mv;
            unpaced += whip.rows[k].mv;
            first = first < 0.0 ? t : first;
            ++windows;
        }
        const double cap = best * (t - first + (1.0 + W::PaceConfig{}.floor) * longest);
        EXPECT_LE(dealt, cap) << whip.name << " " << what;
        EXPECT_GT(unpaced, cap) << whip.name << " " << what
                                << ": the budget alone lets more through";
        std::printf("  %s, %s: %d windows over %.0f s (%d below the floor): %.0f dealt, the best "
                    "move's pace %.0f (x%.3f), unpaced %.0f\n",
                    std::string(whip.name).c_str(), what, windows, t - first, below_floor, dealt,
                    cap, dealt / cap, unpaced);
    }
}

TEST(WhipPace, NoMixOfMovesTheBudgetLetsThroughBeatsTheBestMovesRate) {
    // The default combo budget (two charges) at the presenter's whip refills: every window that
    // registers gets at least the floor.
    RandomMixKeepsTheBestMovesRate(M::BudgetConfig{}, 0.6, true, "the default budget");
}

TEST(WhipPace, NoBudgetNorMoreChargesBeatTheBestMovesRate) {
    // melee_budget=0, melee_budget_charges=8, a quarter of the refills: windows far closer than the
    // floor's share of a recovery. A meter that gave each the floor while its clock stood still
    // (owing no more than the floor) dealt up to 2.3 times the game's rate; each gets only the room
    // left on the clock now.
    M::BudgetConfig off{};
    off.enabled = false;
    RandomMixKeepsTheBestMovesRate(off, 0.2, false, "no budget");
    M::BudgetConfig eight{};
    eight.charges = M::BudgetMaxCharges;
    RandomMixKeepsTheBestMovesRate(eight, 0.3, false, "8 charges");
    M::BudgetConfig quick{};
    quick.charges = M::BudgetMaxCharges;
    quick.scale = 0.25f;
    RandomMixKeepsTheBestMovesRate(quick, 0.3, false, "8 charges, a quarter of the refills");
}

TEST(WhipPace, WindowsAnyCloserKeepTheGamesRate) {
    // Lights every `d` s, the budget not in the way: past the first few, each deals its interval's
    // share of a recovery (d / r): the game's own rate at any interval, no more (the old meter's
    // free floor: 2.3 times the cane's rate at 0.05 s apart) and no less.
    const W::PaceConfig c{};
    for (const W::WhipMoves& whip : W::Whips) {
        const double r = W::WhipRecover(whip.variation, 0);
        for (const double d : {1.0 / 90.0, 0.05, 0.08, 0.10, 0.115, 0.185, 0.317, 0.5, 1.0}) {
            W::PaceMeter m;
            double dealt = 0.0;
            constexpr int N = 400;
            for (int i = 0; i < N; ++i) {
                const float share = m.Pay(Ns(3.0 + i * d), static_cast<float>(r));
                ASSERT_GE(share, 0.0f) << whip.name << " every " << d << " s";
                ASSERT_LE(share, 1.0f) << whip.name << " every " << d << " s";
                if (i >= 40) {
                    EXPECT_NEAR(share, std::min(d / r, 1.0), 2e-3)
                        << whip.name << " every " << d << " s, window " << i;
                }
                dealt += share;
            }
            const double game = (N - 1) * d / r; // full hits at the game's pace in that time
            EXPECT_LE(dealt, std::max(game, N - 1.0) + 1.0 + c.floor + 1e-3)
                << whip.name << " every " << d << " s";
            EXPECT_GE(dealt, std::min(game, N - 1.0) - 1e-3) << whip.name << " every " << d << " s";
            if (d < 0.2) {
                std::printf("  %s lights every %.3f s: x%.3f the game's rate\n",
                            std::string(whip.name).c_str(), d, dealt / (game + 1.0));
            }
        }
    }
}

TEST(WhipPace, AFlurryOnTopOfAHeavyAddsNothingTheClockHasNoRoomFor) {
    // A heavy, then 40 lights 11 ms apart (no budget to stop them): the heavy's recovery runs
    // further ahead than a light's recovery and its floor, so they add nothing (the old meter gave
    // each the floor: the Beast Cutter's 130 motion value became about 700 in 0.44 s). A light one
    // light's recovery before the heavy's ends gets the floor again, and once that is paid, full.
    const W::PaceConfig c{};
    for (const W::WhipMoves& whip : W::Whips) {
        const float rl = W::WhipRecover(whip.variation, 0);
        const float rh = W::WhipRecover(whip.variation, 1);
        W::PaceMeter m;
        EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), rh), 1.0f) << whip.name;
        double lights = 0.0;
        for (int i = 1; i <= 40; ++i) {
            lights += m.Pay(Ns(1.0 + 0.011 * i), rl);
        }
        EXPECT_NEAR(lights, 0.0, 1e-6) << whip.name;
        EXPECT_NEAR(m.Pay(Ns(1.0 + rh - rl), rl), c.floor, 1e-4f) << whip.name;
        EXPECT_NEAR(m.Pay(Ns(1.0 + rh + c.floor * rl), rl), 1.0f, 1e-4f) << whip.name;
        std::printf("  %s: a heavy and 40 lights 11 ms apart: %.0f motion value (unpaced %d)\n",
                    std::string(whip.name).c_str(), whip.rows[1].mv + lights * whip.rows[0].mv,
                    whip.rows[1].mv + 40 * whip.rows[0].mv);
    }
}

TEST(WhipPace, WithoutARecoveryNothingIsPaced) {
    const float r = W::WhipRecover(2200, 0);
    W::PaceMeter m;
    EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), r), 1.0f);
    EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), 0.0f), 1.0f) << "not a whip's move (a blade: WhipRecover 0)";
    EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), std::nanf("")), 1.0f);
    EXPECT_FLOAT_EQ(m.Pay(Ns(1.0), -1.0f), 1.0f);
    EXPECT_NEAR(m.Pay(Ns(1.0 + 0.5 * r), r), 0.5f, 1e-4f) << "those never touched the meter";
    EXPECT_FLOAT_EQ(W::WhipRecover(700, 0), 0.0f);
    // No floor: exactly the share of the recovery.
    W::PaceMeter exact;
    const W::PaceConfig none{0.0f};
    EXPECT_FLOAT_EQ(exact.Pay(Ns(2.0), r, none), 1.0f);
    EXPECT_NEAR(exact.Pay(Ns(2.0 + 0.05 * r), r, none), 0.05f, 1e-4f);
}

std::array<float, 3> CircleVelocity(float t, float rev_s, float radius) {
    const float w = 2.0f * 3.14159265f * rev_s;
    const float a = w * t;
    return {-radius * w * std::sin(a), 0.0f, radius * w * std::cos(a)};
}

TEST(WhipPace, ASpinDealsTheGamesDamagePerSecond) {
    // An overhead spin at 3.15 rev/s for 20 s, nothing but the spin (no stroke of the classifier):
    // each revolution opens a light window paid from the combo budget at the presenter's whip
    // refills, and the chain hits on every one that was paid (an enemy always in reach). Unpaced,
    // every paid revolution was a full light hit: the Threaded Cane about 2.4 times its own rate.
    for (const std::int32_t variation : {2200, 2400}) {
        M::WindowTracker tracker;
        const M::TrackerConfig tc{};
        W::SpinCounter spin;
        const W::SpinConfig sc{};
        W::PaceMeter pace;
        const float r = W::WhipRecover(variation, 0);
        const float dt = 1.0f / 90.0f;
        constexpr float Seconds = 20.0f;
        int paid = 0, unpaid = 0;
        double dealt = 0.0, first_s = -1.0, last_s = 0.0;
        for (int step = 0; static_cast<float>(step) * dt < Seconds; ++step) {
            const float t = static_cast<float>(step) * dt;
            const auto v = CircleVelocity(t, 3.15f, 0.25f);
            M::TrackerInput in{};
            in.dt = dt;
            in.swings = true;
            in.hand_v = {v[0], v[1], v[2]};
            in.hand_speed = std::hypot(v[0], v[2]);
            in.tip_speed = 12.0f;
            in.recover = {PresenterRecover(variation, 0), PresenterRecover(variation, 1),
                          PresenterRecover(variation, 2)};
            M::TrackerOutput out = tracker.Step(in, tc);
            const bool rev = spin.Step(v[0], v[1], v[2], dt, sc);
            if (out.opened) {
                spin.Restart();
            } else if (rev) {
                out = tracker.OpenRevolution(in, tc);
            }
            if (!out.opened) {
                continue;
            }
            const M::Window& w = tracker.Current();
            ASSERT_EQ(w.kind, M::MoveKind::Light);
            if (w.unpaid) {
                ++unpaid; // no attack: nothing to hit with
                continue;
            }
            ++paid;
            const float share = pace.Pay(Ns(t), r);
            EXPECT_GE(share, W::PaceConfig{}.floor);
            EXPECT_LE(share, 1.0f);
            dealt += share;
            first_s = first_s < 0.0 ? t : first_s;
            last_s = t;
        }
        const double game = (last_s - first_s) / r + 1.0; // the game's own light lashes in that time
        EXPECT_GT(paid, 1.25 * game) << variation << ": revolutions register quicker than the game";
        EXPECT_LE(dealt, game + W::PaceConfig{}.floor + 1e-3) << variation;
        EXPECT_GE(dealt, game - 1e-3) << variation << ": a spin keeps up the game's pace";
        std::printf("  spin %d: %d revolutions paid (%d not), %.2f full hits in %.1f s "
                    "(the game's pace: %.2f; unpaced: %d)\n",
                    variation, paid, unpaid, dealt, last_s - first_s, game, paid);
    }
}

TEST(WhipPace, CracksDealTheGamesDamagePerSecond) {
    // Wrist cracks every 0.5 s for 12 s on the Threaded Cane, the swing gate counting none: each
    // is a light window paid at the presenter's refill (0.317 s, so every one registers), and each
    // hurts. Unpaced: 24 full hits in 12 s, 1.5 times the cane's own 0.767 s.
    M::WindowTracker tracker;
    const M::TrackerConfig tc{};
    W::CrackGate gate;
    const W::CrackConfig cc{};
    W::PaceMeter pace;
    const float r = W::WhipRecover(2200, 0);
    const float dt = 1.0f / 90.0f;
    int paid = 0;
    double dealt = 0.0, first_s = -1.0, last_s = 0.0;
    float last_close = -10.0f;
    for (int step = 0; static_cast<float>(step) * dt < 12.0f; ++step) {
        const float t = static_cast<float>(step) * dt;
        const bool surge = std::fmod(t, 0.5f) < 0.2f; // the tip fast for 0.2 s of every 0.5 s
        M::TrackerInput in{};
        in.dt = dt;
        in.swings = true;
        in.tip_speed = surge ? 7.0f : 1.0f;
        in.blade_rate = surge ? 9.0f : 0.5f;
        in.recover = {PresenterRecover(2200, 0), PresenterRecover(2200, 1),
                      PresenterRecover(2200, 2)};
        M::TrackerOutput out = tracker.Step(in, tc);
        if (out.closed) {
            last_close = t;
        }
        const M::Window& w = tracker.Current();
        W::CrackInput ci{};
        ci.dt = dt;
        ci.swings = true;
        ci.window_open = w.open;
        ci.window_committed = w.committed;
        ci.since_close_s = t - last_close;
        ci.lash = 1;
        ci.asked_out = true;
        ci.out = 1.0f;
        ci.tip = surge ? 20.0f : 2.0f;
        ci.swing_speed = in.tip_speed;
        ci.v_drive = 5.5f;
        if (!gate.Step(ci, cc)) {
            continue;
        }
        out = tracker.OpenRevolution(in, tc, "a whip crack");
        ASSERT_TRUE(out.opened);
        if (tracker.Current().unpaid) {
            continue;
        }
        ++paid;
        dealt += pace.Pay(Ns(t), r);
        first_s = first_s < 0.0 ? t : first_s;
        last_s = t;
    }
    const double game = (last_s - first_s) / r + 1.0;
    EXPECT_EQ(paid, 24) << "every crack registers (the refill is quicker than the cracks)";
    EXPECT_LE(dealt, game + W::PaceConfig{}.floor + 1e-3);
    EXPECT_GE(dealt, game - 1e-3);
    std::printf("  cracks: %d paid, %.2f full hits (the game's pace: %.2f)\n", paid, dealt, game);
}

} // namespace
