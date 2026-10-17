// SPDX-License-Identifier: GPL-2.0-or-later

// Item gestures (the user's 2026-10-06 "yes and yes", on top of quick_hold_use): CRUSH a coldblood,
// Madman's Knowledge, the Blood Dreg or Vermin in the fist (squeeze the other button hard), SHAKE
// a bell (three back-and-forth strokes), and squeeze-only Queenly Blood. Bottled blood
// is drunk; the regular Blood Vial retains its dedicated injection path. core/vr/quick_items.h: GestureOf, StabOf, CrushGate, ShakeGate and the Controller. Real
// gestures fire; a normal hold, a walk, a turn, a throw or a tumbling wrist never does.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"

namespace {

namespace Q = VR::Quick;

constexpr float Pi = 3.14159265f;

// ---- Which items -------------------------------------------------------------------------------

TEST(QuickGestures, Tables) {
    for (const int g : {1510, 1511, 1519, 1590, 1594, 1500, 1501, 704, 705, 1700}) {
        EXPECT_TRUE(Q::GestureOf(g) == Q::GestureKind::Crush);
        EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::Consume); // the hold is the fallback
    }
    for (const int g : {200, 205, 225, 111}) {
        EXPECT_TRUE(Q::GestureOf(g) == Q::GestureKind::Shake);
        EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::Rite);
    }
    EXPECT_TRUE(Q::GestureOf(Q::ChoirBell) == Q::GestureKind::Shake); // opt-in (shake_choir)
    EXPECT_TRUE(Q::HoldOf(Q::ChoirBell) == Q::HoldKind::None);
    // No gesture: the marks, the music box, the ash, the cords, the papers, the bloods, the rest.
    for (const int g : {100, 1400, 700, 1330, 4320, 4323, 1300, 1310, 1320, 701, 703, 1000, 1100,
                        1120, 1200, 1410, 2000, 2090, 1411, 1412, 120, -1}) {
        EXPECT_TRUE(Q::GestureOf(g) == Q::GestureKind::None);
    }
    for (const int g : {701, 702, 703, 705, 706}) {
        EXPECT_FALSE(Q::StabOf(g));
        EXPECT_TRUE(Q::Classify(g).use == Q::UseKind::Instant);
        EXPECT_EQ(Q::HoldOf(g), g == 705 ? Q::HoldKind::Consume : Q::HoldKind::None);
    }
    for (const int g : {700, 704, 707, 1000, 1500, 1510}) {
        EXPECT_FALSE(Q::StabOf(g));
    }
    // Vermin and the Blood Dreg keep the game's own crush clip (Vermin's League SpEffects).
    EXPECT_TRUE(Q::Classify(1700).use == Q::UseKind::Square);
    EXPECT_TRUE(Q::Classify(704).use == Q::UseKind::Square);
    // The goods dump's goodsUseAnim agrees.
    struct Row {
        int goods, anim;
    };
    for (const Row r : {Row{1510, 9}, Row{1500, 9}, Row{704, 9}, Row{1700, 33}, Row{200, 14},
                        Row{205, 14}, Row{225, 14}, Row{111, 15}, Row{2050, 18}, Row{705,28}}) {
        EXPECT_TRUE(Q::GestureAnimAgrees(Q::GestureOf(r.goods), r.anim, r.goods));
    }
    EXPECT_FALSE(Q::GestureAnimAgrees(Q::GestureKind::Crush, 14));
    EXPECT_FALSE(Q::GestureAnimAgrees(Q::GestureKind::Shake, 9));
    EXPECT_TRUE(Q::StabAnimAgrees(28));
    // Never a gesture or a stab on an item eaten, drunk, wiped or thrown.
    for (const std::int32_t g : Q::QuickUsableGoods) {
        if (Q::GestureOf(g) != Q::GestureKind::None || Q::StabOf(g)) {
            EXPECT_TRUE(Q::MouthOf(g) == Q::MouthKind::None);
            EXPECT_FALSE(Q::WipeOf(g));
            EXPECT_TRUE(Q::Classify(g).use != Q::UseKind::Throw);
        }
        EXPECT_FALSE(Q::GestureOf(g) != Q::GestureKind::None && Q::StabOf(g));
    }
}

// ---- CRUSH: the detector -------------------------------------------------------------------------

constexpr double Ct = 1.0 / 90.0;

// Feeds `squeeze(t)` from the take (t = 0) for `seconds`; the time it crushed (-1: never).
double RunCrush(const std::function<float(double)>& squeeze, double seconds,
                const std::function<bool(double)>& arm = {}) {
    Q::CrushConfig c{};
    Q::CrushGate g;
    g.Reset(0.0);
    for (double t = 0.0; t <= seconds + 1e-9; t += Ct) {
        if (g.Update(c, t, squeeze(t), arm ? arm(t) : true)) {
            return t;
        }
    }
    return -1.0;
}

TEST(QuickGestures, CrushFreshSqueeze) {
    // Open, then a firm squeeze from 0.3 s: crushed 0.15 s later.
    const double t = RunCrush([](double t) { return t < 0.3 ? 0.1f : 1.0f; }, 2.0);
    EXPECT_GT(t, 0.44);
    EXPECT_LT(t, 0.48);
    // Squeezed 0.12 s only (then eased off to 0.6): nothing.
    EXPECT_LT(RunCrush([](double t) { return t >= 0.3 && t < 0.42 ? 1.0f : 0.1f; }, 2.0), 0.0);
    // A squeeze that rises over 0.1 s to 0.95 and stays: crushed.
    EXPECT_GT(RunCrush([](double t) {
                  return static_cast<float>(std::clamp((t - 0.4) / 0.1, 0.0, 0.95));
              },
              2.0),
              0.0);
}

TEST(QuickGestures, CrushNeverFromTheTakesFist) {
    // Squeezed at the take (a whole-hand grab) and kept: never.
    EXPECT_LT(RunCrush([](double) { return 1.0f; }, 3.0), 0.0);
    // ... until let go under 0.5 and squeezed again.
    const double t = RunCrush([](double t) { return t < 1.0 || (t >= 1.2 && t < 1.25) ? 1.0f : t < 1.2 ? 0.3f : 1.0f; },
                              3.0);
    EXPECT_GT(t, 1.3);
}

TEST(QuickGestures, CrushNotAHalfSqueeze) {
    // A firm hold of the item at 0.8 (a nervous squeeze) for 3 s: never.
    EXPECT_LT(RunCrush([](double t) { return t < 0.2 ? 0.0f : 0.8f; }, 3.0), 0.0);
    // Wobbling round 0.85 .. 0.92 (above press for a frame, below release never): crushed only
    // once it is above release for hold_s after a press.
    EXPECT_GT(RunCrush([](double t) {
                  return t < 0.2 ? 0.0f : 0.88f + 0.04f * std::sin(static_cast<float>(t) * 40.0f);
              },
              3.0),
              0.0);
    // Dipping under release (0.75) restarts the confirm: 0.1 s squeezes with dips never crush.
    EXPECT_LT(RunCrush([](double t) {
                  return t < 0.2 ? 0.0f : std::fmod(t, 0.15) < 0.1 ? 1.0f : 0.6f;
              },
              3.0),
              0.0);
}

TEST(QuickGestures, CrushWaitsForTheArm) {
    // The game has the arm while it is squeezed: the confirm restarts once it is back.
    const double t = RunCrush([](double t) { return t < 0.3 ? 0.0f : 1.0f; }, 2.0,
                              [](double t) { return t < 0.4 || t > 0.7; });
    EXPECT_GT(t, 0.84);
    EXPECT_LT(t, 0.88);
}

// ---- SHAKE: the detector -------------------------------------------------------------------------

constexpr double St = 1.0 / 124.0;

struct ShakeRun {
    int strikes{0};
    double rung{-1.0};
    std::string detail;
};

// Feeds w(t) (rad/s) and v(t) (m/s, against the head) from the take (t = 0) for `seconds`.
ShakeRun RunShake(const std::function<Q::V3(double)>& w, const std::function<Q::V3(double)>& v,
                  double seconds) {
    Q::ShakeConfig c{};
    Q::ShakeGate g;
    g.Reset(0.0);
    ShakeRun r;
    for (double t = 0.0; t <= seconds + 1e-9; t += St) {
        const Q::ShakeGate::Event e = g.Update(c, t, true, w(t), v(t), true);
        if (e == Q::ShakeGate::Event::Stroke) {
            ++r.strikes;
        } else if (e == Q::ShakeGate::Event::Rung) {
            r.rung = t;
            r.detail = g.Detail(c);
            return r;
        }
    }
    return r;
}

const auto None3 = [](double) { return Q::V3{}; };

// A wrist ring: w = A sin(2 pi f (t - t0)) about the controller's x from t0 (0 before) for `n`
// half-cycles (0: on and on).
std::function<Q::V3(double)> Ring(float a, float f, double t0 = 0.3, int n = 0) {
    return [=](double t) {
        const double s = t - t0;
        if (s < 0.0 || (n > 0 && s > n / (2.0 * f))) {
            return Q::V3{};
        }
        return Q::V3{a * std::sin(2.0f * Pi * f * static_cast<float>(s)), 0.0f, 0.0f};
    };
}

TEST(QuickGestures, ShakeRingsAWristRing) {
    const ShakeRun r = RunShake(Ring(8.0f, 3.0f), None3, 3.0);
    EXPECT_GT(r.rung, 0.0);
    EXPECT_LT(r.rung - 0.3, 0.55); // about half a second of ringing
    EXPECT_EQ(r.strikes, 2);       // two clacks, then the toll
    EXPECT_NE(r.detail.find("by the wrist"), std::string::npos);
    // A gentle 2 Hz ring.
    EXPECT_GT(RunShake(Ring(6.0f, 2.0f), None3, 3.0).rung, 0.0);
    // A fast small one (4 Hz, 7 rad/s).
    EXPECT_GT(RunShake(Ring(7.0f, 4.0f), None3, 3.0).rung, 0.0);
}

TEST(QuickGestures, ShakeNotTooWeakOrTooSlow) {
    EXPECT_LT(RunShake(Ring(3.0f, 3.0f), None3, 5.0).rung, 0.0);  // too weak
    EXPECT_LT(RunShake(Ring(8.0f, 1.2f), None3, 5.0).rung, 0.0);  // a slow wave
    EXPECT_LT(RunShake(Ring(10.0f, 1.0f), None3, 5.0).rung, 0.0); // ... a big one
}

TEST(QuickGestures, ShakeRingsAForearmShake) {
    // A stiff wrist: the hand +-6 cm at 3 Hz (1.2 m/s peaks), the controller turning 3 rad/s.
    const auto v = [](double t) {
        const double s = t - 0.3;
        return s < 0.0 ? Q::V3{}
                       : Q::V3{0.0f, 1.2f * std::sin(2.0f * Pi * 3.0f * static_cast<float>(s)),
                               0.0f};
    };
    const ShakeRun r = RunShake(Ring(3.0f, 3.0f), v, 3.0);
    EXPECT_GT(r.rung, 0.0);
    EXPECT_NE(r.detail.find("by the forearm"), std::string::npos);
}

TEST(QuickGestures, ShakeNeverWalking) {
    // A walking arm swing (0.9 Hz) carrying the bell, the wrist swaying with it: 10 s, nothing.
    const auto v = [](double t) {
        return Q::V3{0.0f, 0.2f * std::sin(2.0f * Pi * 1.8f * static_cast<float>(t)),
                     0.9f * std::sin(2.0f * Pi * 0.9f * static_cast<float>(t))};
    };
    const ShakeRun r = RunShake(Ring(1.5f, 0.9f, 0.0), v, 10.0);
    EXPECT_LT(r.rung, 0.0);
    EXPECT_EQ(r.strikes, 0);
    // A brisk walk (1.2 Hz, the hand 1.1 m/s against the head): no clack either.
    const auto brisk = [](double t) {
        return Q::V3{0.0f, 0.0f, 1.1f * std::sin(2.0f * Pi * 1.2f * static_cast<float>(t))};
    };
    const ShakeRun b = RunShake(None3, brisk, 10.0);
    EXPECT_LT(b.rung, 0.0);
    EXPECT_EQ(b.strikes, 0);
    // The bell taken at the peak of a swing (the hold opens mid-lobe): that lobe never counts.
    const auto mid = [](double t) {
        return Q::V3{0.0f, 0.0f, 1.0f * std::cos(2.0f * Pi * 1.5f * static_cast<float>(t - 0.25))};
    };
    EXPECT_EQ(RunShake(None3, mid, 0.5).strikes, 0);
}

TEST(QuickGestures, ShakeNeverTurningOrLooking) {
    // A slow 90 degree turn of the wrist at 3 rad/s (eased in and out), then back.
    const auto turn = [](double t) {
        const auto lobe = [](double s) {
            return s < 0.0 || s > 0.6 ? 0.0f : 3.0f * std::sin(Pi * static_cast<float>(s / 0.6));
        };
        return Q::V3{0.0f, lobe(t - 0.4) - lobe(t - 1.4), 0.0f};
    };
    EXPECT_LT(RunShake(turn, None3, 3.0).rung, 0.0);
    // Turning it over to look at it: 4 rad/s twice back and forth, slowly (0.5 s each way).
    EXPECT_LT(RunShake(Ring(4.0f, 1.0f, 0.3), None3, 4.0).rung, 0.0);
    // Turning it over fast once (10 rad/s out and back): two strokes, never three.
    const ShakeRun once = RunShake(Ring(10.0f, 3.0f, 0.3, 2), None3, 3.0);
    EXPECT_LT(once.rung, 0.0);
    EXPECT_LE(once.strikes, 2);
}

TEST(QuickGestures, ShakeNeverAThrowMotion) {
    // Wind-up back 0.4 s, the fling 0.12 s, the recoil 0.15 s (wrist and forearm).
    const auto lobe = [](double s, double len, float peak) {
        return s < 0.0 || s > len ? 0.0f : peak * std::sin(Pi * static_cast<float>(s / len));
    };
    const auto w = [&](double t) {
        return Q::V3{lobe(t - 0.5, 0.4, -4.0f) + lobe(t - 0.9, 0.12, 15.0f) +
                         lobe(t - 1.02, 0.15, -5.0f),
                     0.0f, 0.0f};
    };
    const auto v = [&](double t) {
        return Q::V3{0.0f, 0.0f,
                     lobe(t - 0.5, 0.4, 1.5f) + lobe(t - 0.9, 0.12, -8.0f) +
                         lobe(t - 1.02, 0.15, 1.0f)};
    };
    EXPECT_LT(RunShake(w, v, 3.0).rung, 0.0);
    // ... even with a wind-up as fast as a stroke (the strokes lopsided: min_ratio).
    const auto w2 = [&](double t) {
        return Q::V3{lobe(t - 0.5, 0.2, -5.5f) + lobe(t - 0.7, 0.12, 20.0f) +
                         lobe(t - 0.82, 0.15, -5.5f),
                     0.0f, 0.0f};
    };
    EXPECT_LT(RunShake(w2, None3, 3.0).rung, 0.0);
}

TEST(QuickGestures, ShakeNeverATumblingWrist) {
    // The angular velocity's axis circling at 3 Hz, 8 rad/s: no back and forth on one axis.
    const auto w = [](double t) {
        const float a = 2.0f * Pi * 3.0f * static_cast<float>(t);
        return Q::V3{8.0f * std::cos(a), 8.0f * std::sin(a), 0.0f};
    };
    EXPECT_LT(RunShake(w, None3, 3.0).rung, 0.0);
}

TEST(QuickGestures, ShakeIgnoresATrackingJump) {
    // A one-frame 60 rad/s spike in the middle of a ring: neither counted nor breaking it.
    const auto ring = Ring(8.0f, 3.0f);
    const ShakeRun clean = RunShake(ring, None3, 3.0);
    const auto spiky = [&](double t) {
        return std::abs(t - 0.5) < 0.5 * St ? Q::V3{0.0f, 60.0f, 0.0f} : ring(t);
    };
    const ShakeRun r = RunShake(spiky, None3, 3.0);
    EXPECT_GT(r.rung, 0.0);
    EXPECT_NEAR(r.rung, clean.rung, 2.5 * St);
    // A linear jump (30 m/s for a frame) the same.
    const auto jump = [](double t) {
        return std::abs(t - 0.5) < 0.5 * St ? Q::V3{30.0f, 0.0f, 0.0f} : Q::V3{};
    };
    EXPECT_NEAR(RunShake(ring, jump, 3.0).rung, clean.rung, 2.5 * St);
}

TEST(QuickGestures, ShakeAPauseBreaksTheChain) {
    // Two strokes, 0.4 s still, two strokes: never rung.
    const auto a = Ring(8.0f, 3.0f, 0.3, 2);
    const auto b = Ring(8.0f, 3.0f, 0.3 + 1.0 / 3.0 + 0.4, 2);
    const auto w = [&](double t) { return a(t) + b(t); };
    const ShakeRun r = RunShake(w, None3, 3.0);
    EXPECT_LT(r.rung, 0.0);
    EXPECT_EQ(r.strikes, 4);
}

TEST(QuickGestures, ShakeNotFromTheTake) {
    // Shaking from the take on for 0.45 s only (the take's own jerk, min_hold_s): never.
    EXPECT_LT(RunShake(Ring(8.0f, 3.0f, 0.0, 3), None3, 2.0).rung, 0.0);
}

// The review of 2026-10-06: the same at a 72 Hz headset's input rate, and a single flick of the
// wrist with its rebound (a look at the bell, a startle) never rings.
double RunShakeAt(double dt, const std::function<Q::V3(double)>& w,
                  const std::function<Q::V3(double)>& v, double seconds) {
    Q::ShakeConfig c{};
    Q::ShakeGate g;
    g.Reset(0.0);
    for (double t = 0.0; t <= seconds + 1e-9; t += dt) {
        if (g.Update(c, t, true, w(t), v(t), true) == Q::ShakeGate::Event::Rung) {
            return t;
        }
    }
    return -1.0;
}

TEST(QuickGestures, ShakeAt72Hz) {
    constexpr double D = 1.0 / 72.0;
    EXPECT_GT(RunShakeAt(D, Ring(6.0f, 2.0f), None3, 3.0), 0.0); // a gentle ring
    EXPECT_GT(RunShakeAt(D, Ring(7.0f, 4.0f), None3, 3.0), 0.0); // a fast small one
    const auto forearm = [](double t) {
        const double s = t - 0.3;
        return s < 0.0 ? Q::V3{}
                       : Q::V3{0.0f, 1.2f * std::sin(2.0f * Pi * 3.0f * static_cast<float>(s)),
                               0.0f};
    };
    EXPECT_GT(RunShakeAt(D, None3, forearm, 3.0), 0.0);
    const auto brisk = [](double t) {
        return Q::V3{0.0f, 0.0f, 1.1f * std::sin(2.0f * Pi * 1.2f * static_cast<float>(t))};
    };
    EXPECT_LT(RunShakeAt(D, None3, brisk, 10.0), 0.0);
}

TEST(QuickGestures, ShakeNeverAFlickAndItsRebound) {
    // A sharp flick (12 rad/s, 0.12 s), the wrist's rebound (40%, then 16%): two lobes at most.
    const auto lobe = [](double s, double len, float peak) {
        return s < 0.0 || s > len ? 0.0f : peak * std::sin(Pi * static_cast<float>(s / len));
    };
    const auto w = [&](double t) {
        return Q::V3{lobe(t - 0.5, 0.12, 12.0f) + lobe(t - 0.62, 0.12, -4.8f) +
                         lobe(t - 0.74, 0.12, 1.9f),
                     0.0f, 0.0f};
    };
    EXPECT_LT(RunShakeAt(St, w, None3, 2.0), 0.0);
    EXPECT_LT(RunShakeAt(1.0 / 72.0, w, None3, 2.0), 0.0);
}

// ---- The controller ----------------------------------------------------------------------------

constexpr double Dt = 1.0 / 90.0;
const Q::V3 Rest{-0.2f, 1.1f, -0.3f};

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, -1, true}; // Molotov (thrown)
    s.slot[1] = {81, 200, 1, 18, -1, true};  // Beckoning Bell (shake; the game's own ring)
    s.slot[2] = {82, 1510, 9, 20, -1, true}; // Coldblood Dew (crush; UseGoods at once)
    s.slot[3] = {83, 703, 2, 21, -1, true};  // Iosefka's Blood Vial (stab)
    s.slot[4] = {84, 1700, 3, 22, -1, true}; // Vermin (crush; the game's own crush clip)
    s.slot[5] = {85, 2050, 1, 23, -1, true}; // Choir Bell (a hunter tool: at the pull)
    s.selected = 0;
    return s;
}

int Count(const Q::Output& o, Q::Haptic h) {
    int n = 0;
    for (int i = 0; i < o.haptic_count; ++i) {
        n += o.haptics[static_cast<std::size_t>(i)] == h ? 1 : 0;
    }
    return n;
}

struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{};
    int uses = 0, selects = 0, squares = 0;
    int done = 0, crunch = 0, strike = 0, toll = 0, inject = 0, putback = 0, deny = 0, use = 0;
    float max_hum = 0.0f;
    float max_progress = -1.0f;
    bool l2_free_while_held = false;
    std::string log;
    double t = 100.0;
    double t0 = 0.0; // the shake's start (Rig::Wrist)
    std::function<Q::V3(double)> wrist;
    std::function<Q::V3(double)> forearm;

    explicit Rig(int scheme = 2) {
        cfg.left_scheme = scheme;
        cfg.hold_use = true;
        cfg.mouth = true;
        cfg.crush = true;
        cfg.shake = true;
        cfg.stab = true;
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = Rest;
        in.snapshot_fresh = true;
        in.snapshot = Slots();
    }
    const Q::Output& Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += Dt;
            in.t = t;
            in.dt = static_cast<float>(Dt);
            in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
            in.hand_wv = wrist ? wrist(t - t0) : Q::V3{};
            in.hand_v = forearm ? forearm(t - t0) : Q::V3{};
            c.Update(cfg, in, out);
            if (out.send) {
                sent = out.request;
                uses += sent.action == Q::Action::Use ? 1 : 0;
                selects += sent.action == Q::Action::None ? 1 : 0;
            }
            squares += out.square_press ? 1 : 0;
            done += Count(out, Q::Haptic::HoldDone);
            crunch += Count(out, Q::Haptic::Crunch);
            strike += Count(out, Q::Haptic::BellStrike);
            toll += Count(out, Q::Haptic::BellToll);
            inject += Count(out, Q::Haptic::Inject);
            putback += Count(out, Q::Haptic::PutBack);
            deny += Count(out, Q::Haptic::Deny);
            use += Count(out, Q::Haptic::Use);
            max_hum = std::max(max_hum, out.hold_hum);
            max_progress = std::max(max_progress, out.hold_progress);
            if (out.held_goods >= 0 && !out.block_l2) {
                l2_free_while_held = true;
            }
            log += out.log.empty() ? "" : out.log + "\n";
            in.x_edge = false;
            in.y = false;
            in.dodge = false;
            in.stab = false;
        }
        return out;
    }
    void Trigger(float v) {
        in.trigger = v;
        in.trigger_raw = v;
        in.trigger_latch = in.trigger_latch ? v > 0.25f : v >= 0.45f;
    }
    void Grip(float v) {
        in.grip = v;
    }
    void Hover(int slot) {
        in.hover_fresh = true;
        in.hover.open_serial = c.OpenSerial();
        in.hover.placed = true;
        in.hover.slot = static_cast<std::int8_t>(slot);
    }
    // left_scheme=2: the grip opens the arc (`keep_grip`: and stays squeezed), the hand on `slot`
    // past the dwell, the trigger takes.
    void Take2(int slot, bool keep_grip = false) {
        Grip(1.0f);
        Step();
        if (!keep_grip) {
            Grip(0.0f);
        }
        Step();
        Hover(slot);
        Step(20);
        Trigger(1.0f);
        Step();
        in.hover.slot = -1;
    }
    // left_scheme=1: Y opens, the hand on `slot`, the grip takes (and stays squeezed).
    void Take1(int slot, float trigger = 0.0f) {
        in.y = true;
        Step();
        Hover(slot);
        Step(20);
        Grip(1.0f);
        Trigger(trigger);
        Step();
        in.hover.slot = -1;
    }
    void Shake(std::function<Q::V3(double)> w, std::function<Q::V3(double)> v = {}) {
        t0 = t;
        wrist = std::move(w);
        forearm = std::move(v);
    }
    void Still() {
        wrist = {};
        forearm = {};
    }
    void Result(Q::Outcome o) {
        in.have_result = true;
        in.result = {sent.serial, o, "", sent.goods, 9, o == Q::Outcome::Used ? 8 : 9};
        Step();
        in.have_result = false;
    }
    [[nodiscard]] Q::Phase Phase() const {
        return c.GetPhase();
    }
};

int Frames(double s) {
    return static_cast<int>(std::lround(s / Dt));
}

// ---- CRUSH in the controller ---------------------------------------------------------------------

TEST(QuickGestures, CrushAColdbloodWithTheGrip) {
    Rig r;
    r.Take2(2);
    EXPECT_NE(r.log.find("or CRUSH it: squeeze the grip hard"), std::string::npos);
    r.Step(Frames(0.25));
    r.Grip(1.0f);
    r.Step(Frames(0.1));
    EXPECT_EQ(r.uses, 0);
    EXPECT_GE(r.out.hold_hum, 0.35f); // the strain
    r.Step(Frames(0.08));
    EXPECT_EQ(r.uses, 1); // crushed: the game's use at once
    EXPECT_EQ(r.sent.goods, 1510);
    EXPECT_EQ(r.crunch, 1);
    EXPECT_EQ(r.done, 0);
    EXPECT_EQ(r.putback, 0); // the grip did not put it back
    EXPECT_GT(r.out.scene.hold_done_ns, 0); // the flash
    EXPECT_EQ(r.out.scene.hold_from_ns, 0);
    EXPECT_GT(r.max_hum, 0.6f);
    EXPECT_NE(r.log.find("CRUSH (the grip squeezed to 1.00"), std::string::npos);
    EXPECT_TRUE(r.Phase() == Q::Phase::Waiting);
    r.Result(Q::Outcome::Used);
    EXPECT_EQ(r.use, 0); // the crunch was its feel
    EXPECT_FALSE(r.l2_free_while_held);
    EXPECT_TRUE(r.out.block_l2); // the trigger still held is not the gun until let go
}

TEST(QuickGestures, CrushVerminPlaysTheGamesOwnCrush) {
    Rig r;
    r.Take2(4);
    r.Step(Frames(0.3));
    r.Grip(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.crunch, 1);
    EXPECT_EQ(r.uses, 0); // never UseGoods: select + Square (its League SpEffects)
    r.in.snapshot.selected = 4;
    r.Step();
    EXPECT_EQ(r.squares, 1);
}

TEST(QuickGestures, CrushNeverWithTheGripHeldFromTheOpen) {
    Rig r;
    r.Take2(2, true); // the grip still squeezed from opening the arc
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.Step(Frames(1.0));
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.crunch, 0);
    EXPECT_EQ(r.putback, 0);
    r.Grip(0.2f); // let go ...
    r.Step(3);
    r.Grip(1.0f); // ... and squeezed: crushed
    r.Step(Frames(0.2));
    EXPECT_EQ(r.crunch, 1);
    EXPECT_EQ(r.uses, 1);
}

TEST(QuickGestures, AFirmHoldIsNoCrush) {
    // The grip at 0.8 all along: the ring completes at 1.5 s (the hold's thump, not the crunch).
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.2));
    r.Grip(0.8f);
    r.Step(Frames(1.2));
    EXPECT_EQ(r.uses, 0);
    r.Step(Frames(0.15));
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.done, 1);
    EXPECT_EQ(r.crunch, 0);
}

TEST(QuickGestures, AQuickGripTapIsNoCrush) {
    // The review of 2026-10-06: a tap of the grip (0.1 s at full) on a coldblood neither crushes
    // it nor puts it back; it stays in the hand, the ring filling on.
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.3));
    r.Grip(1.0f);
    r.Step(Frames(0.1));
    r.Grip(0.0f);
    r.Step(Frames(0.3));
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.crunch, 0);
    EXPECT_EQ(r.putback, 0);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.Grip(1.0f); // a real squeeze then crushes it
    r.Step(Frames(0.2));
    EXPECT_EQ(r.crunch, 1);
    EXPECT_EQ(r.uses, 1);
}

TEST(QuickGestures, GripStillPutsABellBack) {
    Rig r;
    r.Take2(1);
    r.Step(Frames(0.4));
    r.Grip(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.putback, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
    EXPECT_EQ(r.uses + r.squares + r.toll, 0);
}

TEST(QuickGestures, CrushOffTheGripPutsBack) {
    Rig r;
    r.cfg.crush = false;
    r.Take2(2);
    r.Step(Frames(0.4));
    r.Grip(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.putback, 1);
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.crunch, 0);
}

TEST(QuickGestures, SchemeOneCrushWithTheTrigger) {
    Rig r(1);
    r.Take1(2);
    EXPECT_NE(r.log.find("or CRUSH it: squeeze the trigger hard"), std::string::npos);
    r.Step(Frames(0.3));
    r.Trigger(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.crunch, 1);
    EXPECT_FALSE(r.l2_free_while_held); // the gun never fires while it is in the hand
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.out.block_l2); // the trigger still pulled: not the gun until let go
    r.Trigger(0.0f);
    r.Step(2);
    EXPECT_FALSE(r.out.block_l2);
}

TEST(QuickGestures, SchemeOneTheGrabsFistIsNoCrush) {
    Rig r(1);
    r.Take1(2, 1.0f); // the trigger pulled with the grip's grab (a whole-hand grab)
    r.Step(Frames(1.0));
    EXPECT_EQ(r.crunch, 0);
    EXPECT_EQ(r.uses, 0);
    r.Trigger(0.1f);
    r.Step(2);
    r.Trigger(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.crunch, 1);
}

TEST(QuickGestures, ACrushInTheCooldownWaits) {
    Rig r;
    r.cfg.use_cooldown_s = 3.0f;
    r.Take2(2);
    r.Step(Frames(0.25));
    r.Grip(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Used);
    r.Grip(0.0f);
    r.Trigger(0.0f);
    r.Step(2);
    r.in.snapshot.slot[2].qty = 8;
    r.Take2(2);
    r.Step(Frames(0.25));
    r.Grip(1.0f);
    r.Step(Frames(0.3));
    EXPECT_EQ(r.uses, 1); // crushed, waiting out the cooldown
    EXPECT_NEAR(r.out.hold_progress, 1.0f, 1e-6f);
    EXPECT_NE(r.log.find("waits for the cooldown"), std::string::npos);
    r.Step(Frames(2.5));
    EXPECT_EQ(r.uses, 2);
    EXPECT_EQ(r.crunch, 2);
}

TEST(QuickGestures, ACrushTheGameRefusesIsADeny) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.25));
    r.in.snapshot.slot[2].usable = false;
    r.Grip(1.0f);
    r.Step(Frames(0.2));
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.crunch, 0);
    EXPECT_EQ(r.deny, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
}

// ---- SHAKE in the controller ---------------------------------------------------------------------

TEST(QuickGestures, ShakeABellRingsIt) {
    Rig r;
    r.Take2(1);
    EXPECT_NE(r.log.find("or SHAKE it: three strokes"), std::string::npos);
    r.Step(Frames(0.3));
    r.Shake(Ring(9.0f, 3.0f, 0.0));
    r.Step(Frames(0.6));
    EXPECT_EQ(r.toll, 1);
    EXPECT_EQ(r.strike, 2);
    EXPECT_EQ(r.done, 0);
    EXPECT_EQ(r.uses, 0);            // never UseGoods: the game's own ring
    EXPECT_GE(r.selects, 1);         // the slot selected ...
    EXPECT_GT(r.out.scene.hold_done_ns, 0);
    EXPECT_NE(r.log.find("SHAKE (3 strokes in"), std::string::npos);
    r.Still();
    r.in.snapshot.selected = 1;
    r.Step();
    EXPECT_EQ(r.squares, 1); // ... and Square
}

TEST(QuickGestures, ShakeStepsTheRing) {
    Rig r;
    r.Take2(1);
    r.Step(Frames(0.3));
    // Two strokes (one full swing out and back), then still.
    r.Shake(Ring(9.0f, 3.0f, 0.0, 2));
    r.Step(Frames(0.34));
    EXPECT_EQ(r.strike, 2);
    EXPECT_GE(r.out.hold_progress, 0.66f); // two thirds at least
    EXPECT_LE(r.out.hold_hum, 0.10f);      // the hum ducked under the strikes
    r.Still();
    r.Step(Frames(0.3));
    EXPECT_LT(r.out.hold_progress, 0.65f); // the strokes stopped: back to the time held
    EXPECT_NE(r.log.find("the strokes stopped after 2: the ring back"), std::string::npos);
    EXPECT_EQ(r.toll, 0);
    // The hold still completes on its own time (1.5 s from the take).
    r.Step(Frames(0.6));
    EXPECT_EQ(r.done, 1);
}

TEST(QuickGestures, ABellCarriedWalkingAndTurningDoesNothing) {
    Rig r;
    r.Take2(1);
    // Walking (the arm swinging 0.9 Hz), the head turning (its velocity cancels in the forearm
    // channel), the bell turned over to look at it: 1.4 s, under the hold.
    r.Shake([](double t) {
                return Q::V3{1.5f * std::sin(2.0f * Pi * 0.9f * static_cast<float>(t)),
                             2.5f * std::sin(2.0f * Pi * 0.6f * static_cast<float>(t)), 0.0f};
            },
            [](double t) {
                return Q::V3{0.0f, 0.1f, 0.9f * std::sin(2.0f * Pi * 0.9f * static_cast<float>(t))};
            });
    r.Step(Frames(1.4));
    EXPECT_EQ(r.strike, 0);
    EXPECT_EQ(r.toll, 0);
    EXPECT_EQ(r.uses + r.squares, 0);
    EXPECT_LT(r.max_progress, 0.95f);
    r.Trigger(0.0f);
    r.Step();
    EXPECT_EQ(r.putback, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
}

TEST(QuickGestures, ABellTheGameRefusesGivesNoToll) {
    Rig r;
    r.in.snapshot.slot[1].usable = true;
    r.Take2(1);
    r.Step(Frames(0.3));
    r.in.snapshot.slot[1].usable = false; // offline: a multiplayer bell
    r.Shake(Ring(9.0f, 3.0f, 0.0));
    r.Step(Frames(0.6));
    EXPECT_EQ(r.toll, 0);
    EXPECT_EQ(r.deny, 1);
    EXPECT_EQ(r.squares, 0);
    EXPECT_NE(r.log.find("rung (SHAKE (3 strokes in"), std::string::npos); // how, for tuning
}

TEST(QuickGestures, ShakeOffIsTheHoldOnly) {
    Rig r;
    r.cfg.shake = false;
    r.Take2(1);
    r.Step(Frames(0.3));
    r.Shake(Ring(9.0f, 3.0f, 0.0));
    r.Step(Frames(0.6));
    EXPECT_EQ(r.strike + r.toll, 0);
    EXPECT_EQ(r.squares, 0);
    r.Step(Frames(0.7));
    EXPECT_EQ(r.done, 1);
}

// quick_shake_choir_bell defaults to 1 (the user's 2026-10-06 "The Choir Bell - shake"); 0 keeps
// the old use at the pull.
TEST(QuickGestures, TheChoirBellIsRungByDefault) {
    {
        Rig r;
        r.cfg.shake_choir = false; // used at the pull (the game's own ring), no hold
        r.Take2(5);
        EXPECT_TRUE(r.Phase() == Q::Phase::SquareWait || r.Phase() == Q::Phase::Idle);
        EXPECT_EQ(r.log.find("HOLD to use"), std::string::npos);
    }
    {
        Rig r; // the default: held and rung
        r.Take2(5);
        EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
        EXPECT_NE(r.log.find("HOLD to use (rite)"), std::string::npos);
        r.Step(Frames(0.3));
        r.Shake(Ring(9.0f, 3.0f, 0.0));
        r.Step(Frames(0.6));
        EXPECT_EQ(r.toll, 1);
    }
}

TEST(QuickGestures, AHitWhileShakingCancels) {
    Rig r;
    r.Take2(1);
    r.Step(Frames(0.3));
    r.Shake(Ring(9.0f, 3.0f, 0.0));
    r.Step(Frames(0.2));
    r.in.arm_weight = 0.2f; // a hit's stagger
    r.Step(Frames(0.4));
    EXPECT_EQ(r.toll, 0);
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_EQ(r.squares, 0);
}

// Queenly Blood requires a deliberate fresh squeeze, never a timed hold or a stab.
TEST(QuickGestures, QueenlyBloodSqueezeOnlyInBothSchemes) {
    for(int scheme : {1,2}) for(bool timed_hold : {false,true}) {
        Rig r(scheme);r.cfg.hold_use=timed_hold;
        r.in.snapshot.slot[2].goods=705;
        if(scheme==1) r.Take1(2); else r.Take2(2);
        ASSERT_EQ(r.out.held_goods,705);
        EXPECT_FALSE(r.out.stab_item);
        r.in.stab=true;r.in.stab_zone="torso";
        r.Step(Frames(3));
        EXPECT_EQ(r.uses,0);EXPECT_EQ(r.inject,0);
        EXPECT_LE(r.out.hold_progress,0.0f);
        EXPECT_LT(r.out.use_whoosh,0.0f);
        EXPECT_EQ(r.out.hold_hum,0.0f);
        if(scheme==1) r.Trigger(1); else r.Grip(1);
        r.Step(Frames(.2));
        ASSERT_EQ(r.uses,1);
        EXPECT_EQ(r.sent.goods,705);EXPECT_EQ(r.sent.stab,0);
        EXPECT_EQ(r.crunch,1);
    }
}
TEST(QuickGestures, QueenlyBloodCannotBeConsumedByWaitingWithCrushDisabled) {
    Rig r;r.cfg.crush=false;r.in.snapshot.slot[2].goods=705;r.Take2(2);
    r.Step(Frames(4));EXPECT_EQ(r.uses,0);
}

} // namespace

TEST(QuickGestures, BellStrokesExpireAcrossUnobservedTrackingTime) {
    for (bool report_lost : {false, true}) {
        Q::ShakeConfig cfg;
        Q::ShakeGate gate;
        gate.Reset(0);
        const auto ring = Ring(8, 3, 0.3, 2);
        for (double t = 0; t < 0.64; t += St) {
            gate.Update(cfg, t, true, ring(t), {}, true);
        }
        ASSERT_EQ(gate.Strokes(), 2);
        if (report_lost) {
            gate.Update(cfg, 0.65, false, {}, {}, true);
        }
        // The next sample can be valid even though tracking was absent for > lost_s.
        gate.Update(cfg, 0.79, true, {}, {}, true);
        EXPECT_EQ(gate.Strokes(), 0) << report_lost;
        const auto resumed = Ring(8, 3, 0.80, 1);
        int rings = 0;
        for (double t = 0.80; t < 1.05; t += St) {
            rings += gate.Update(cfg, t, true, resumed(t), {}, true) == Q::ShakeGate::Event::Rung;
        }
        EXPECT_EQ(rings, 0) << report_lost;
    }
}

TEST(QuickGestures, UseWhooshBuildsAndStopsWithTheHeldItemInBothSchemes) {
    for (int scheme : {1, 2}) {
        Rig r(scheme);
        if (scheme == 1) r.Take1(2); else r.Take2(2);
        r.Step(Frames(0.25));
        const float early = r.out.use_whoosh;
        EXPECT_GE(early, 0);
        r.Step(Frames(0.5));
        EXPECT_GT(r.out.use_whoosh, early);
        r.in.hand_valid = false;
        r.Step();
        EXPECT_LT(r.out.use_whoosh, 0);
        r.in.hand_valid = true;
        r.Step();
        EXPECT_GT(r.out.use_whoosh, early);
        if (scheme == 1) r.Grip(0); else r.Trigger(0);
        r.Step();
        EXPECT_LT(r.out.use_whoosh, 0);
        EXPECT_EQ(r.uses, 0);
    }
}

TEST(QuickGestures, CrushWhooshAcceleratesThenStopsOnUseOrDenial) {
    for (bool usable : {true, false}) {
        Rig r;
        r.Take2(2);
        r.Step(Frames(0.25));
        const float held = r.out.use_whoosh;
        r.in.snapshot.slot[2].usable = usable;
        r.Grip(1);
        r.Step(Frames(0.1));
        EXPECT_GT(r.out.use_whoosh, held + 0.3f);
        r.Step(Frames(0.08));
        EXPECT_LT(r.out.use_whoosh, 0);
        EXPECT_EQ(r.uses, usable ? 1 : 0);
        EXPECT_EQ(r.deny, usable ? 0 : 1);
    }
}

TEST(QuickGestures, UseWhooshStopsOnMenuAndLeavesBellAudioAlone) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.25));
    ASSERT_GE(r.out.use_whoosh, 0);
    r.in.gameplay = false;
    r.Step();
    EXPECT_LT(r.out.use_whoosh, 0);
    Rig bell;
    bell.Take2(0);
    bell.Step(Frames(0.3));
    EXPECT_LT(bell.out.use_whoosh, 0);
}
