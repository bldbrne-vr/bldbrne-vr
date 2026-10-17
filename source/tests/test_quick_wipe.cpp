// SPDX-License-Identifier: GPL-2.0-or-later

// Wipe to apply (quick_use_wipe, the user's 2026-10-06 request: "having to apply the firepaper on
// the weapon itself"): Fire Paper, Bolt Paper and the Empty Phantasm Shell taken from the
// quick-item arc stay in the hunter's left hand and are applied when wiped along the right
// weapon's blade; let go of away from it they go back, nothing used. core/vr/quick_items.h:
// WipeOf, WipeMeasure, WipeDetector and the Controller in both left-hand layouts.
// The tests of the first version (2026-10-06) run its stroke (WipeClassic, kept selectable
// through the settings); "Easier" below: the user's 2026-10-08 "make applying the fire paper a
// little easier (like easier to find the spot where we need to rub)" (the defaults: a wider
// zone, a shorter stroke, the paper's own move, the find cue).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"
#include "core/vr/quick_settings.h"

namespace {

namespace Q = VR::Quick;

constexpr double Dt = 1.0 / 90.0;   // the presenter's input frames
constexpr int SampleEvery = 3;      // the game's rendered frames (30 a second) give a sample

// A 50 cm blade (the Saw Cleaver's capsule) held out in front, base to tip along -Z.
const Q::V3 Base{0.25f, 1.10f, -0.35f};
const Q::V3 Tip{0.25f, 1.10f, -0.85f};
constexpr float BladeM = 0.50f;
const Q::V3 Rest{-0.2f, 1.1f, -0.3f};

// The paper `along` m from the base, `off` across the blade (x: sideways, y: up).
Q::V3 OnBlade(float along, float off_x = 0.0f, float off_y = 0.03f) {
    return Base + Q::V3{off_x, off_y, -along};
}

// ---- The detector on synthetic paths ---------------------------------------------------------

struct Path {
    Q::WipeDetector d;
    Q::WipeConfig c = Q::WipeClassic();
    double t = 10.0;
    bool applied = false;
    double applied_t = -1.0;
    float best_progress = 0.0f;
    int teeth = 0;
    bool touched = false;
    explicit Path(double held_for = 1.0) {
        d.Reset(t - held_for);
    }
    // One game sample with the paper at `p`.
    void Sample(Q::V3 p, bool arms = true) {
        t += Dt * SampleEvery;
        const Q::WipeContact w = Q::WipeMeasure(p, Base, Tip);
        const bool done = d.Update(c, t, w.valid, arms, w.along, w.dist, w.length,
                                   {w.off[0], w.off[1], w.off[2]});
        touched |= d.Touching();
        best_progress = std::max(best_progress, d.Progress());
        teeth += d.TakeTeeth();
        if (done && !applied) {
            applied = true;
            applied_t = t;
            d.Consume();
        }
    }
    // From `a` to `b` over `seconds` (minimum jerk), sampled at the game's rate.
    void Move(Q::V3 a, Q::V3 b, double seconds, bool arms = true) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds / (Dt * SampleEvery))));
        for (int i = 0; i <= n; ++i) {
            const double s = static_cast<double>(i) / n;
            const double q = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
            Sample(a + (b - a) * static_cast<float>(q), arms);
        }
    }
};

TEST(QuickWipe, Classification) {
    for (const int g : {1300, 1310, 1320}) {
        EXPECT_TRUE(Q::WipeOf(g));
        EXPECT_TRUE(Q::Classify(g).use == Q::UseKind::Instant); // the game's UseGoods at once
        EXPECT_TRUE(Q::MouthOf(g) == Q::MouthKind::None);
        EXPECT_TRUE(Q::Classify(g).use != Q::UseKind::Throw);
    }
    // The ash goes on the gun, the rest is eaten, drunk, crushed, thrown, injected or rung.
    for (const int g : {1330, 1000, 1100, 1101, 1110, 1120, 2030, 1200, 1210, 1230, 1410, 1500,
                        1510, 701, 703, 100, 200, 1411, 4320}) {
        EXPECT_FALSE(Q::WipeOf(g));
    }
    // The goods params' goodsUseAnim (the 1.09 goods dump): 1 = the blade-wipe clip.
    struct Row {
        int goods, anim;
    };
    for (const Row r : {Row{1300, 1}, Row{1310, 1}, Row{1320, 1}, Row{1330, 5}, Row{1110, 0},
                        Row{1120, 10}, Row{1510, 9}, Row{701, 28}, Row{1200, 3}, Row{100, 16}}) {
        EXPECT_EQ(Q::WipeOf(r.goods), Q::WipeOfAnim(r.anim));
    }
}

TEST(QuickWipe, Measure) {
    const Q::WipeContact w = Q::WipeMeasure(OnBlade(0.2f, 0.03f, 0.04f), Base, Tip);
    EXPECT_TRUE(w.valid);
    EXPECT_NEAR(w.length, BladeM, 1e-5f);
    EXPECT_NEAR(w.along, 0.2f, 1e-5f);
    EXPECT_NEAR(w.dist, 0.05f, 1e-5f);
    // Past the tip: measured from the tip itself.
    const Q::WipeContact past = Q::WipeMeasure(OnBlade(0.53f, 0.0f, 0.04f), Base, Tip);
    EXPECT_NEAR(past.dist, 0.05f, 1e-4f);
    EXPECT_NEAR(past.along, 0.53f, 1e-5f);
    // No blade (both ends at one point): invalid.
    EXPECT_FALSE(Q::WipeMeasure(Base, Base, Base).valid);
}

TEST(QuickWipe, AFullWipeApplies) {
    // Base to tip, 3 cm over the flat, in 0.4 s: 40 cm of a 50 cm blade.
    Path p;
    p.Sample(OnBlade(0.04f));
    p.Move(OnBlade(0.04f), OnBlade(0.44f), 0.4);
    EXPECT_TRUE(p.applied);
    EXPECT_FLOAT_EQ(p.best_progress, 1.0f);
    // A rasp tooth every 3 cm.
    EXPECT_GE(p.teeth, 10);
    EXPECT_LE(p.teeth, 14);
    // The other way (tip to base) and lower on the flat, a little sideways: also.
    Path r;
    r.Move(OnBlade(0.48f, 0.02f, -0.03f), OnBlade(0.18f, -0.01f, -0.035f), 0.35);
    EXPECT_TRUE(r.applied);
    // Applied as soon as 40% (20 cm) is covered, not at the end of the stroke.
    Path e;
    e.Move(OnBlade(0.02f), OnBlade(0.47f), 0.5);
    ASSERT_TRUE(e.applied);
    EXPECT_LT(e.applied_t, e.t - 0.1);
}

TEST(QuickWipe, ProgressRisesWithTheStroke) {
    Path p;
    p.Sample(OnBlade(0.05f));
    float last = 0.0f;
    bool rising = true;
    for (int i = 1; i <= 6; ++i) {
        p.Sample(OnBlade(0.05f + 0.03f * static_cast<float>(i)));
        rising &= p.d.Progress() >= last;
        last = p.d.Progress();
    }
    EXPECT_TRUE(rising);
    EXPECT_NEAR(last, 0.18f / 0.20f, 0.02f);
    EXPECT_FALSE(p.applied);
}

TEST(QuickWipe, ATapDoesNothing) {
    // The paper laid on the blade and held there half a second, a few mm of jitter.
    Path p;
    for (int i = 0; i < 15; ++i) {
        p.Sample(OnBlade(0.25f + 0.004f * static_cast<float>(i % 3), 0.003f * (i % 2), 0.03f));
    }
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    EXPECT_LT(p.best_progress, 0.1f);
    // A tap: on and off.
    Path q;
    q.Sample(Rest);
    q.Sample(OnBlade(0.2f));
    q.Sample(Rest);
    q.Sample(OnBlade(0.21f));
    q.Sample(Rest);
    EXPECT_FALSE(q.applied);
}

TEST(QuickWipe, ACrossCutDoesNothing) {
    // Straight across the blade at its middle, 20 cm through it in 0.2 s.
    Path p;
    p.Move(OnBlade(0.25f, -0.10f, 0.0f), OnBlade(0.25f, 0.10f, 0.0f), 0.2);
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    // At 45 degrees through it: 14 cm along it, as much across.
    Path d;
    d.Move(OnBlade(0.10f, -0.15f, 0.0f), OnBlade(0.40f, 0.15f, 0.0f), 0.3);
    EXPECT_FALSE(d.applied);
    // A slash along the blade, a hand's swing (50 cm in 60 ms): quicker than any wipe.
    Path s;
    s.Sample(Rest);
    s.Sample(OnBlade(0.02f));
    s.Sample(OnBlade(0.48f));
    s.Sample(Rest);
    EXPECT_FALSE(s.applied);
}

TEST(QuickWipe, TooFarDoesNothing) {
    // The whole stroke 8 cm off the blade (beyond the 7 cm reach): never on it.
    Path p;
    p.Move(OnBlade(0.04f, 0.0f, 0.08f), OnBlade(0.46f, 0.0f, 0.08f), 0.4);
    EXPECT_FALSE(p.touched);
    EXPECT_FALSE(p.applied);
    // Along the line of the blade but beyond its tip (in the air past it).
    Path q;
    q.Move(OnBlade(0.60f), OnBlade(1.0f), 0.4);
    EXPECT_FALSE(q.touched);
    EXPECT_FALSE(q.applied);
    // Started on it (4 cm) and lifted to 11 cm: on it until past 10 cm (the hysteresis), then off.
    Path h;
    h.Sample(OnBlade(0.1f, 0.0f, 0.04f));
    h.Sample(OnBlade(0.12f, 0.0f, 0.095f));
    EXPECT_TRUE(h.d.Touching());
    h.Sample(OnBlade(0.14f, 0.0f, 0.11f));
    EXPECT_FALSE(h.d.Touching());
    // A fist laid on the flat at the blade's edge (4 cm over it, 3.5 cm sideways: 5.3 cm from the
    // axis) wipes.
    Path f;
    f.Move(OnBlade(0.04f, 0.035f, 0.04f), OnBlade(0.40f, 0.035f, 0.04f), 0.45);
    EXPECT_TRUE(f.applied);
}

TEST(QuickWipe, NeitherTooSlowNorAScrub) {
    // 20 cm in 2 s: never 40% within 0.8 s.
    Path p;
    p.Move(OnBlade(0.10f), OnBlade(0.30f), 2.0);
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    // Scrubbing back and forth over 6 cm for a second.
    Path s;
    for (int k = 0; k < 4; ++k) {
        s.Move(OnBlade(0.20f), OnBlade(0.26f), 0.12);
        s.Move(OnBlade(0.26f), OnBlade(0.20f), 0.12);
    }
    EXPECT_FALSE(s.applied);
    EXPECT_LT(s.best_progress, 0.4f);
    // ... but a turn back then a full stroke the other way applies.
    s.Move(OnBlade(0.20f), OnBlade(0.45f), 0.3);
    EXPECT_TRUE(s.applied);
}

TEST(QuickWipe, OnlyYourArmsAndAfterTheTake) {
    // The game animating an arm (a hit, a dodge): nothing.
    Path p;
    p.Move(OnBlade(0.04f), OnBlade(0.46f), 0.4, false);
    EXPECT_FALSE(p.touched);
    EXPECT_FALSE(p.applied);
    // Taken just now: the stroke completes before min_hold_s (0.13 s), and applies once that is
    // over, still on the run.
    Path e(0.0);
    e.Move(OnBlade(0.04f), OnBlade(0.30f), 0.1);
    EXPECT_FALSE(e.applied);
    e.Move(OnBlade(0.30f), OnBlade(0.36f), 0.15);
    EXPECT_TRUE(e.applied);
}

TEST(QuickWipe, ASlowDeliberateStrokeApplies) {
    // 22 cm at a careful 0.3 m/s (0.75 s, minimum jerk): within the 0.8 s window.
    Path p;
    p.Move(OnBlade(0.05f), OnBlade(0.27f), 0.75);
    EXPECT_TRUE(p.applied);
}

TEST(QuickWipe, ALongBladeNeedsAtMost25cm) {
    // A greatsword's 1.1 m capsule: 40% would be 44 cm; 25 cm is a full wipe of the hand.
    Q::WipeDetector d;
    Q::WipeConfig c = Q::WipeClassic();
    d.Reset(0.0);
    bool done = false;
    for (int i = 0; i <= 9; ++i) {
        done |= d.Update(c, 1.0 + 0.05 * i, true, true, 0.10f + 0.03f * static_cast<float>(i),
                         0.03f, 1.10f, {});
    }
    EXPECT_NEAR(d.Required(), 0.25f, 1e-5f);
    EXPECT_TRUE(done); // 27 cm in 0.45 s
}

TEST(QuickWipe, ALongWindowKeepsItsSamples) {
    // quick_wipe_window_ms=2000 at the game's 60 samples a second: a 20 cm stroke over 1.8 s
    // counts (the samples of the whole window are kept).
    Q::WipeDetector d;
    Q::WipeConfig c = Q::WipeClassic();
    c.window_s = 2.0f;
    d.Reset(0.0);
    bool done = false;
    const int n = 108; // 1.8 s at 60 a second
    for (int i = 0; i <= n; ++i) {
        done |= d.Update(c, 1.0 + i / 60.0, true, true,
                         0.05f + 0.20f * static_cast<float>(i) / n, 0.03f, 0.50f, {});
    }
    EXPECT_TRUE(done);
}

TEST(QuickWipe, AShortBladeNeedsLessButAtLeast8cm) {
    // The Beast Claw's 25 cm: 40% = 10 cm.
    Q::WipeDetector d;
    Q::WipeConfig c = Q::WipeClassic();
    d.Reset(0.0);
    double t = 1.0;
    bool done = false;
    for (int i = 0; i <= 6; ++i) {
        t += 0.033;
        done |= d.Update(c, t, true, true, 0.03f + 0.02f * static_cast<float>(i), 0.02f, 0.25f, {});
    }
    EXPECT_TRUE(done); // 12 cm
    EXPECT_NEAR(d.Required(), 0.10f, 1e-5f);
    // A 15 cm one: 8 cm, not 6.
    d.Reset(0.0);
    d.Update(c, 2.0, true, true, 0.02f, 0.02f, 0.15f, {});
    EXPECT_FALSE(d.Update(c, 2.2, true, true, 0.08f, 0.02f, 0.15f, {}));
    EXPECT_NEAR(d.Required(), 0.08f, 1e-5f);
    EXPECT_TRUE(d.Update(c, 2.3, true, true, 0.11f, 0.02f, 0.15f, {}));
    // No blade shorter than 10 cm.
    d.Reset(0.0);
    EXPECT_FALSE(d.Update(c, 3.0, true, true, 0.02f, 0.01f, 0.08f, {}));
    EXPECT_FALSE(d.Touching());
}

// ---- The Controller ----------------------------------------------------------------------------

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, -1, true}; // Molotov
    s.slot[1] = {81, 1300, 3, 30, -1, true}; // Fire Paper
    s.slot[2] = {82, 1320, 3, 31, -1, true}; // Bolt Paper
    s.slot[3] = {83, 1110, 3, 20, -1, true}; // Beast Blood Pellet (eaten at the mouth)
    s.slot[4] = {84, 1510, 3, 23, -1, true}; // Coldblood Dew
    s.slot[5] = {85, 1310, 1, 32, -1, true}; // Empty Phantasm Shell
    s.selected = 0;
    return s;
}

bool Has(const Q::Output& o, Q::Haptic h) {
    for (int i = 0; i < o.haptic_count; ++i) {
        if (o.haptics[static_cast<std::size_t>(i)] == h) {
            return true;
        }
    }
    return false;
}

// The wipe's versions a Rig runs: the first (2026-10-06), the morning's one-way stroke
// (2026-10-08) and the rub (the defaults).
constexpr int Classic = 0, Easier = 1, RubMode = 2;

struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{};
    int uses = 0;
    int wipe_uses = 0;
    int throws = 0;
    std::string log;
    bool ignite = false, use_feel = false, deny = false, putback = false, touched = false;
    float progress = 0.0f;
    int teeth = 0;
    float near_max = 0.0f;    // the find cue's highest
    bool near_touch = false;  // ... ever on while it touched
    std::vector<float> nears; // ... each frame
    double t = 100.0;
    int frame = 0;
    Q::V3 paper = Rest; // where the paper is (the game's sample)
    bool contact = true; // the render-camera hook sends samples
    bool blade = true;   // ... with a blade in them
    std::uint32_t serial = 0;

    explicit Rig(int scheme = 2, bool wipe = true, int mode = Classic) {
        cfg.left_scheme = scheme;
        cfg.wipe = wipe;
        if (mode == Classic) {
            cfg.wipe_cfg = Q::WipeClassic(); // the first version's stroke (its tests)
        } else if (mode == Easier) {
            cfg.wipe_cfg = Q::WipeEasier(); // the morning's one-way stroke (its tests)
        }
        cfg.mouth = true;
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = Rest;
        in.mouth_valid = true;
        in.mouth = Q::MouthPoint({0.0f, 1.6f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.08f, 0.08f);
        in.snapshot_fresh = true;
        in.snapshot = Slots();
    }
    const Q::Output& Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += Dt;
            in.t = t;
            in.dt = static_cast<float>(Dt);
            in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
            // The render-camera hook's sample of the paper against the blade (the held item's).
            if (contact && ++frame % SampleEvery == 0) {
                // (The paper's card at the paper, lying flat over the blade facing up, 8 cm
                // square: the rub's contact; the earlier versions measure the paper point.)
                Q::WipeContact w = Q::WipeMeasureCard(paper, paper, {1.0f, 0.0f, 0.0f},
                                                      {0.0f, 0.0f, -1.0f}, 0.04f, 0.04f, Base,
                                                      blade ? Tip : Base);
                w.serial = ++serial;
                w.steady_ns = static_cast<std::int64_t>(t * 1e9);
                w.goods = out.held_goods;
                w.arms = true;
                in.wipe = w;
            }
            in.wipe_fresh = contact && t - static_cast<double>(in.wipe.steady_ns) * 1e-9 < 0.15;
            c.Update(cfg, in, out);
            if (out.send) {
                sent = out.request;
                uses += sent.action == Q::Action::Use ? 1 : 0;
                throws += sent.action == Q::Action::Throw ? 1 : 0;
                wipe_uses += sent.wipe != 0 ? 1 : 0;
            }
            ignite |= Has(out, Q::Haptic::Ignite);
            use_feel |= Has(out, Q::Haptic::Use);
            deny |= Has(out, Q::Haptic::Deny);
            putback |= Has(out, Q::Haptic::PutBack);
            touched |= out.wipe_touch;
            near_max = std::max(near_max, out.wipe_near);
            near_touch |= out.wipe_touch && out.wipe_near > 0.0f;
            nears.push_back(out.wipe_near);
            progress = std::max(progress, out.wipe_progress);
            teeth += out.wipe_teeth;
            log += out.log.empty() ? "" : out.log + "\n";
            in.x_edge = false;
            in.y = false;
        }
        return out;
    }
    void Trigger(float v) {
        in.trigger = v;
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
    void Take2(int slot) {
        Grip(1.0f);
        Step();
        Grip(0.0f);
        Step();
        Hover(slot);
        Step(20);
        Trigger(1.0f);
        Step();
        in.hover.slot = -1;
    }
    void Take1(int slot) {
        in.y = true;
        Step();
        Hover(slot);
        Step(20);
        Grip(1.0f);
        Step();
        in.hover.slot = -1;
    }
    // The paper (and the hand with it) from `a` to `b` over `seconds`, minimum jerk.
    void Wipe(Q::V3 a, Q::V3 b, double seconds) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds / Dt)));
        for (int i = 0; i <= n; ++i) {
            const double s = static_cast<double>(i) / n;
            const double q = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
            paper = a + (b - a) * static_cast<float>(q);
            in.hand_p = paper + Q::V3{0.0f, -0.03f, 0.0f};
            Step();
        }
    }
    void Result(Q::Outcome o) {
        in.have_result = true;
        in.result = {sent.serial, o, "", sent.goods, 3, o == Q::Outcome::Used ? 2 : 3};
        Step();
        in.have_result = false;
    }
};

TEST(QuickWipe, Scheme2TakeHoldWipeApplied) {
    Rig r;
    r.Take2(1); // Fire Paper
    r.Step(10);
    // Nothing used at the pull: in the hunter's hand while the trigger is held.
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1300);
    EXPECT_NE(r.log.find("WIPE it along the blade"), std::string::npos);
    // Brought to the blade's base (on it: the rasp starts), then wiped to the tip.
    r.Wipe(Rest, OnBlade(0.03f), 0.4);
    r.Step(6);
    EXPECT_TRUE(r.out.wipe_touch);
    EXPECT_EQ(r.uses, 0);
    r.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_TRUE(r.touched);
    EXPECT_GT(r.teeth, 6);
    EXPECT_GT(r.progress, 0.8f); // (the frame it completes is the thump's)
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 1);
    EXPECT_EQ(r.sent.goods, 1300);
    EXPECT_TRUE(r.sent.action == Q::Action::Use);
    EXPECT_NE(r.log.find("USE goods=1300 wiped along the blade"), std::string::npos);
    // Still in the hand until the game takes it; the rasp is over.
    EXPECT_EQ(r.out.held_goods, 1300);
    EXPECT_FALSE(r.out.wipe_touch);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.ignite);
    EXPECT_FALSE(r.use_feel);
    EXPECT_EQ(r.out.held_goods, -1); // gone from the hand
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
    // The trigger still held is no shot until let go.
    r.Step(5);
    EXPECT_TRUE(r.out.block_l2);
    r.Trigger(0.0f);
    r.Step(3);
    EXPECT_FALSE(r.out.block_l2);
}

TEST(QuickWipe, ReleasedAwayGoesBack) {
    Rig r;
    r.Take2(2); // Bolt Paper
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.1f, 0.0f, 0.15f), 0.3); // near the blade, never on it
    r.Trigger(0.0f);
    r.Step(3);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.putback);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Open); // back on the arc
    EXPECT_NE(r.log.find("trigger let go away from the blade: back"), std::string::npos);
    // Let go of on the blade mid-stroke: back too, nothing used.
    Rig m;
    m.Take2(2);
    m.Step(10);
    m.Wipe(Rest, OnBlade(0.05f), 0.3);
    m.Wipe(OnBlade(0.05f), OnBlade(0.15f), 0.2);
    m.Trigger(0.0f);
    m.Step(3);
    EXPECT_EQ(m.uses, 0);
    EXPECT_NE(m.log.find("on the blade, the stroke unfinished: back"), std::string::npos);
}

TEST(QuickWipe, OffIsTheOldUseAtThePull) {
    Rig r(2, false);
    r.Take2(1);
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 0);
    EXPECT_EQ(r.sent.goods, 1300);
}

TEST(QuickWipe, ThrowablesAndMouthItemsUnaffected) {
    // A Molotov wiped along the blade stays in the hand, and its release throws.
    Rig r;
    r.Take2(0);
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.03f), 0.3);
    r.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_EQ(r.uses, 0);
    EXPECT_FALSE(r.touched);
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Trigger(0.0f);
    r.Step(4);
    EXPECT_EQ(r.throws, 1);
    // The pellet: wiped along the blade nothing; at the mouth eaten.
    Rig p;
    p.Take2(3);
    p.Step(10);
    p.Wipe(Rest, OnBlade(0.03f), 0.3);
    p.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_EQ(p.uses, 0);
    EXPECT_FALSE(p.touched);
    p.in.hand_p = p.in.mouth;
    p.in.hand_v = {};
    p.Step(30);
    EXPECT_EQ(p.uses, 1);
    EXPECT_EQ(p.wipe_uses, 0);
}

TEST(QuickWipe, NotAHoldItemWhenWiped) {
    // quick_hold_use on: the papers are the wipe's (no ring), the coldblood the ring's.
    Rig r;
    r.cfg.hold_use = true;
    r.Take2(1);
    r.Step(200); // 2.2 s in the hand, away from the blade
    EXPECT_EQ(r.uses, 0);
    EXPECT_LT(r.out.hold_progress, 0.0f);
    EXPECT_EQ(r.out.held_goods, 1300);
    // With the wipe off they are the ring's again.
    Rig h(2, false);
    h.cfg.hold_use = true;
    h.Take2(1);
    h.Step(200);
    EXPECT_EQ(h.uses, 1);
}

TEST(QuickWipe, NoBladeFallsBackToTheRing) {
    // Nothing to wipe it on (fists, an unknown weapon): after half a second the ring takes over
    // and uses it; never unusable in the hand.
    Rig r;
    r.cfg.hold_use = true;
    r.blade = false;
    r.Take2(1);
    r.Step(40); // 0.44 s: still the wipe's
    EXPECT_EQ(r.log.find("nothing to wipe it on"), std::string::npos);
    r.Step(10);
    EXPECT_EQ(r.uses, 0);
    EXPECT_NE(r.log.find("WIPE: nothing to wipe it on (no blade known): HOLD to use instead"),
              std::string::npos);
    r.Step(150); // the ring's 1.5 s
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 0);
    // No samples at all (the hunter's model not read): the same.
    Rig s;
    s.cfg.hold_use = true;
    s.contact = false;
    s.Take2(2);
    s.Step(200);
    EXPECT_EQ(s.uses, 1);
    EXPECT_NE(s.log.find("(no sample of it)"), std::string::npos);
    // A blade there: never the ring (2.2 s held away from it).
    Rig b;
    b.cfg.hold_use = true;
    b.Take2(1);
    b.Step(200);
    EXPECT_EQ(b.uses, 0);
    EXPECT_EQ(b.log.find("nothing to wipe it on"), std::string::npos);
}

TEST(QuickWipe, TwoHandedGripFirstLetsGo) {
    Rig r;
    r.Take2(1);
    r.Step(10);
    r.in.two_hand = true;
    r.Wipe(Rest, OnBlade(0.03f), 0.3);
    r.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_EQ(r.uses, 0);
    EXPECT_FALSE(r.out.wipe_touch);
    r.in.two_hand = false;
    r.Wipe(OnBlade(0.45f), OnBlade(0.05f), 0.4);
    EXPECT_EQ(r.uses, 1);
}

TEST(QuickWipe, RefusedStaysInTheHand) {
    // The game says it cannot be used now at the stroke's end: the deny tick, still held.
    Rig r;
    r.Take2(1);
    r.Step(10);
    r.in.snapshot.slot[1].usable = false;
    r.Wipe(Rest, OnBlade(0.03f), 0.3);
    r.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.deny);
    EXPECT_EQ(r.out.held_goods, 1300);
    // Usable again: a new stroke applies; refused by the game after the use went out, it stays
    // in the hand (the trigger held) and the next stroke applies.
    r.in.snapshot.slot[1].usable = true;
    r.Wipe(OnBlade(0.45f), OnBlade(0.03f), 0.4);
    ASSERT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Denied);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1300);
    r.Step(50); // the cooldown
    r.Wipe(OnBlade(0.03f), OnBlade(0.45f), 0.4);
    EXPECT_EQ(r.uses, 2);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.ignite);
}

TEST(QuickWipe, LostContactStopsTheRasp) {
    Rig r;
    r.Take2(1);
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.05f), 0.3);
    r.Step(4);
    EXPECT_TRUE(r.out.wipe_touch);
    r.contact = false; // no rendered frames (a loading screen)
    r.Step(20);
    EXPECT_FALSE(r.out.wipe_touch);
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickWipe, AHitchMidStrokeKeepsIt) {
    // The game renders nothing for 0.25 s halfway along the stroke (a hitch; the mock's eye dump):
    // the stroke goes on from where the samples resume (before: a 0.15 s gap ended it).
    Rig r;
    r.Take2(1);
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.03f), 0.4);
    r.Step(4);
    r.Wipe(OnBlade(0.03f), OnBlade(0.12f), 0.2);
    r.contact = false;
    r.Wipe(OnBlade(0.12f), OnBlade(0.24f), 0.25);
    EXPECT_FALSE(r.out.wipe_touch); // the rasp pauses
    EXPECT_EQ(r.uses, 0);
    r.contact = true;
    r.Wipe(OnBlade(0.24f), OnBlade(0.30f), 0.12);
    EXPECT_EQ(r.wipe_uses, 1);
    // A longer one (0.6 s) ends it: a new stroke from where the samples resume.
    Rig g;
    g.Take2(1);
    g.Step(10);
    g.Wipe(Rest, OnBlade(0.03f), 0.4);
    g.Wipe(OnBlade(0.03f), OnBlade(0.12f), 0.2);
    g.contact = false;
    g.Wipe(OnBlade(0.12f), OnBlade(0.20f), 0.6);
    g.contact = true;
    g.Wipe(OnBlade(0.20f), OnBlade(0.30f), 0.15);
    EXPECT_EQ(g.uses, 0);
}

TEST(QuickWipe, ContextSwitchPutsItAway) {
    Rig r;
    r.Take2(1);
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.05f), 0.3);
    r.in.gameplay = false; // a menu
    r.Step(3);
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_FALSE(r.out.wipe_touch);
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickWipe, Scheme1GripHoldsWipeApplies) {
    Rig r(1);
    r.Take1(5); // the Empty Phantasm Shell
    r.Step(10);
    EXPECT_EQ(r.out.held_goods, 1310);
    r.Wipe(Rest, OnBlade(0.04f), 0.3);
    r.Wipe(OnBlade(0.04f), OnBlade(0.44f), 0.4);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 1);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.ignite);
    // Let go of away from the blade: put away, nothing used.
    Rig a(1);
    a.Take1(1);
    a.Step(10);
    a.Grip(0.0f);
    a.Step(3);
    EXPECT_EQ(a.uses, 0);
    EXPECT_TRUE(a.putback);
    EXPECT_NE(a.log.find("grip released away from the blade"), std::string::npos);
}

// ---- Easier (the user's 2026-10-08 request; the defaults) ---------------------------------------

// The detector with the defaults, the paper's own place in every sample, a blade that can move.
struct Stroke {
    Q::WipeDetector d;
    Q::WipeConfig c = Q::WipeEasier();
    double t = 10.0;
    bool applied = false;
    double applied_t = -1.0;
    bool touched = false;
    float best_progress = 0.0f;
    float own = -1.0f;
    Q::V3 base = Base;
    Q::V3 tip = Tip;
    explicit Stroke(double held_for = 1.0) {
        d.Reset(t - held_for);
    }
    // One game sample (60 a second) with the paper at `p`.
    void Sample(Q::V3 p) {
        t += 1.0 / 60.0;
        const Q::WipeContact w = Q::WipeMeasure(p, base, tip);
        const bool done = d.Update(c, t, w, true);
        touched |= d.Touching();
        best_progress = std::max(best_progress, d.Progress());
        if (done && !applied) {
            applied = true;
            applied_t = t;
            own = d.OwnShare();
            d.Consume();
        }
    }
    static double Ease(int i, int n) {
        const double s = static_cast<double>(i) / n;
        return 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
    }
    // The paper from `a` to `b` over `seconds` (minimum jerk), the blade held still.
    void Move(Q::V3 a, Q::V3 b, double seconds) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds * 60.0)));
        for (int i = 0; i <= n; ++i) {
            Sample(a + (b - a) * static_cast<float>(Ease(i, n)));
        }
    }
    // The blade moved by `from` -> `to` (both ends) over `seconds`, the paper from `p` to `p_to`.
    void MoveBlade(Q::V3 from, Q::V3 to, double seconds, Q::V3 p, Q::V3 p_to) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds * 60.0)));
        for (int i = 0; i <= n; ++i) {
            const float q = static_cast<float>(Ease(i, n));
            const Q::V3 shift = from + (to - from) * q;
            base = Base + shift;
            tip = Tip + shift;
            Sample(p + (p_to - p) * q);
        }
    }
};

TEST(QuickWipe, EasierDefaults) {
    const Q::WipeConfig c = Q::WipeEasier();
    EXPECT_FALSE(c.rub); // (quick_wipe_rub=0; the rub is the default now)
    EXPECT_FLOAT_EQ(c.reach_m, 0.12f);
    EXPECT_FLOAT_EQ(c.exit_m, 0.16f);
    EXPECT_FLOAT_EQ(c.end_margin_m, 0.06f);
    EXPECT_FLOAT_EQ(c.share, 0.30f);
    EXPECT_FLOAT_EQ(c.min_travel_m, 0.07f);
    EXPECT_FLOAT_EQ(c.window_s, 1.0f);
    EXPECT_FLOAT_EQ(c.own_share, 0.40f);
    EXPECT_FLOAT_EQ(c.cue_m, 0.30f);
    EXPECT_FLOAT_EQ(c.max_travel_m, 0.20f); // the merge's skeptic: the long forms (25 cm before)
    EXPECT_FLOAT_EQ(c.max_turn_deg, 35.0f); // ... and a swing turns the blade
    // The first version, selectable (quick_wipe_reach_cm=7 ...).
    constexpr Q::WipeConfig o = Q::WipeClassic();
    EXPECT_FLOAT_EQ(o.reach_m, 0.07f);
    EXPECT_FLOAT_EQ(o.exit_m, 0.10f);
    EXPECT_FLOAT_EQ(o.end_margin_m, 0.04f);
    EXPECT_FLOAT_EQ(o.share, 0.40f);
    EXPECT_FLOAT_EQ(o.min_travel_m, 0.08f);
    EXPECT_FLOAT_EQ(o.max_travel_m, 0.25f);
    EXPECT_FLOAT_EQ(o.window_s, 0.80f);
    EXPECT_FLOAT_EQ(o.own_share, 0.0f);
    EXPECT_FLOAT_EQ(o.max_turn_deg, 0.0f);
    EXPECT_FLOAT_EQ(o.cue_m, 0.0f);
}

// ---- The merge's skeptic (2026-10-08) ----------------------------------------------------------

// `v` turned `ang` radians about `axis` (Rodrigues).
Q::V3 Turned(Q::V3 v, Q::V3 axis, float ang) {
    const Q::V3 k = Q::Normalize(axis, {0.0f, 1.0f, 0.0f});
    const float c = std::cos(ang);
    const float s = std::sin(ang);
    return v * c + Q::Cross(k, v) * s + k * (Q::Dot(k, v) * (1.0f - c));
}

// A forehand: the blade (50 cm) turned `wrist_deg` about the fist and the torso `torso_deg`
// round the spine (both hands with it) over `seconds`, the paper `side` m beside the fist and
// `up` m up the blade's way at the start: true when it applied.
bool SwingWithThePaperByTheHilt(const Q::WipeConfig& c, float wrist_deg, float torso_deg,
                                double seconds, float side, float up) {
    Q::WipeDetector d;
    double t = 10.0;
    d.Reset(t - 1.0);
    const Q::V3 spine{0.0f, 1.1f, 0.0f};
    const Q::V3 grip0{0.25f, 1.10f, -0.35f};
    const Q::V3 dir0 = Q::Normalize(Q::V3{-0.8f, 0.0f, -0.6f}, {0.0f, 0.0f, -1.0f});
    const Q::V3 paper0 = grip0 + Q::V3{-side, 0.02f, -up};
    bool applied = false;
    const auto sample = [&](Q::V3 paper, Q::V3 grip, Q::V3 dir) {
        t += 1.0 / 60.0;
        applied |= d.Update(c, t, Q::WipeMeasure(paper, grip, grip + dir * 0.5f), true);
    };
    for (int i = 0; i < 20; ++i) {
        sample(paper0, grip0, dir0); // held there a third of a second
    }
    const int n = std::max(1, static_cast<int>(std::lround(seconds * 60.0)));
    for (int i = 0; i <= n; ++i) {
        const float e = static_cast<float>(Stroke::Ease(i, n));
        const float torso = -torso_deg * 0.017453293f * e;
        const Q::V3 up_axis{0.0f, 1.0f, 0.0f};
        const Q::V3 grip = spine + Turned(grip0 - spine, up_axis, torso);
        const Q::V3 dir = Turned(Turned(dir0, up_axis, wrist_deg * 0.017453293f * e), up_axis, torso);
        sample(spine + Turned(paper0 - spine, up_axis, torso), grip, dir);
    }
    for (int i = 0; i < 20; ++i) {
        sample(spine + Turned(paper0 - spine, {0.0f, 1.0f, 0.0f}, -torso_deg * 0.017453293f),
               spine + Turned(grip0 - spine, {0.0f, 1.0f, 0.0f}, -torso_deg * 0.017453293f),
               Turned(dir0, {0.0f, 1.0f, 0.0f}, (wrist_deg - torso_deg) * 0.017453293f));
    }
    return applied;
}

TEST(QuickWipe, EasierASwingWithThePaperByTheHiltIsRefused) {
    // The skeptic's find: the paper held by the hilt (6-12 cm beside the fist, 10 cm up the
    // blade), a forehand (110 degrees in 0.25 s) with the torso turning 30 degrees (it carries
    // the paper hand round too: 45% of the stroke "the paper's own"): without the turn rule it
    // went through as a wipe; the first version's 7 cm / 20 cm held.
    const Q::WipeConfig c = Q::WipeEasier();
    Q::WipeConfig no_turn = Q::WipeEasier();
    no_turn.max_turn_deg = 0.0f;
    for (const float side : {0.06f, 0.12f}) {
        EXPECT_FALSE(SwingWithThePaperByTheHilt(c, 110.0f, 30.0f, 0.25, side, 0.10f));
        EXPECT_TRUE(SwingWithThePaperByTheHilt(no_turn, 110.0f, 30.0f, 0.25, side, 0.10f));
        EXPECT_FALSE(SwingWithThePaperByTheHilt(Q::WipeClassic(), 110.0f, 30.0f, 0.25, side, 0.10f));
    }
    // Slower and smaller swings (the torso a third of the wrist), the paper 8 cm beside it.
    for (const float deg : {40.0f, 60.0f, 90.0f}) {
        for (const double s : {0.25, 0.5}) {
            EXPECT_FALSE(SwingWithThePaperByTheHilt(c, deg, deg / 3.0f, s, 0.08f, 0.10f));
        }
    }
    EXPECT_TRUE(SwingWithThePaperByTheHilt(no_turn, 90.0f, 30.0f, 0.5, 0.08f, 0.10f));
}

TEST(QuickWipe, EasierARubWhileTheBladeTiltsApplies) {
    // A 20 cm rub in 0.5 s, 7 cm off the blade, while the blade tilts up to 40 degrees (about
    // its base, in the rub's plane): a wipe holds the blade only roughly still.
    for (const float deg : {0.0f, 15.0f, 30.0f, 40.0f}) {
        Stroke p;
        const int n = 30;
        for (int i = 0; i <= n; ++i) {
            const float e = static_cast<float>(Stroke::Ease(i, n));
            const Q::V3 dir = Turned({0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, deg * 0.017453293f * e);
            p.base = Base;
            p.tip = Base + dir * BladeM;
            const Q::V3 side = Q::Normalize(Q::Cross(dir, {1.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f});
            p.Sample(Base + dir * (0.15f + 0.20f * e) + side * 0.07f);
        }
        EXPECT_TRUE(p.applied);
    }
}

TEST(QuickWipe, EasierTheLongFormsAskAtMost20cm) {
    // The Saw Cleaver's B (its capsule 109 cm) and the Hunter Axe's B (the haft to the head,
    // 160 cm): 30% is past the cap, 20 cm now (the first version's 25: the user's own strokes
    // were 20-21 cm). A slow 30 cm rub (0.15 m/s on average, 2 s) along the Saw Cleaver's B.
    for (const float len : {1.09f, 1.60f}) {
        Stroke p;
        p.tip = Base + Q::V3{0.0f, 0.0f, -len};
        const auto at = [&](float along) { return Base + Q::V3{-0.08f, 0.0f, -along}; };
        p.Sample(at(0.10f));
        p.Move(at(0.10f), at(0.40f), 2.0);
        EXPECT_TRUE(p.applied);
        EXPECT_NEAR(p.d.Required(), 0.20f, 1e-5f);
        Stroke o;
        o.c = Q::WipeClassic();
        o.tip = p.tip;
        o.Move(Base + Q::V3{-0.04f, 0.0f, -0.10f}, Base + Q::V3{-0.04f, 0.0f, -0.40f}, 2.0);
        EXPECT_NEAR(o.d.Required(), 0.25f, 1e-5f); // (the 7 cm zone: 4 cm off)
        EXPECT_FALSE(o.applied);                   // 24 cm in its best 0.8 s ... not 25
    }
}

TEST(QuickWipe, EasierASlowDeliberateRubApplies) {
    // 18 cm at a careful 0.2 m/s (0.9 s), the fist 9 cm off the blade's axis (the user's strokes
    // finished 7.6-10 cm off): out of the first version's 7 cm reach, in the 12 cm one.
    Stroke p;
    p.Sample(OnBlade(0.06f, 0.0f, 0.09f));
    p.Move(OnBlade(0.06f, 0.0f, 0.09f), OnBlade(0.24f, 0.0f, 0.09f), 0.9);
    EXPECT_TRUE(p.applied);
    EXPECT_NEAR(p.own, 1.0f, 0.02f); // all the paper's own move
    Path o; // the first version: never on the blade
    o.Move(OnBlade(0.06f, 0.0f, 0.09f), OnBlade(0.24f, 0.0f, 0.09f), 0.9);
    EXPECT_FALSE(o.touched);
    EXPECT_FALSE(o.applied);
    // ... and an even slower one: 16 cm over 1.0 s, still within the window.
    Stroke q;
    q.Move(OnBlade(0.10f, 0.03f, 0.05f), OnBlade(0.26f, 0.03f, 0.05f), 1.0);
    EXPECT_TRUE(q.applied);
}

TEST(QuickWipe, EasierAShortRubApplies) {
    // 30% of the Saw Cleaver's 50 cm: 15 cm, in 0.3 s (the first version asked for 20).
    Stroke p;
    p.Move(OnBlade(0.20f), OnBlade(0.36f), 0.3);
    EXPECT_TRUE(p.applied);
    EXPECT_NEAR(p.d.Required(), 0.15f, 1e-5f);
    // 12 cm: not yet (80%), nothing used.
    Stroke s;
    s.Move(OnBlade(0.20f), OnBlade(0.32f), 0.3);
    EXPECT_FALSE(s.applied);
    EXPECT_NEAR(s.best_progress, 0.8f, 0.03f);
    // A 20 cm blade: 30% is 6 cm, at least 7.
    Q::WipeDetector d;
    const Q::WipeConfig c = Q::WipeEasier();
    d.Reset(0.0);
    d.Update(c, 1.0, true, true, 0.02f, 0.03f, 0.20f, {});
    EXPECT_NEAR(d.Required(), 0.07f, 1e-5f);
}

TEST(QuickWipe, EasierARubFromTheTipApplies) {
    // From 3 cm past the tip (in the zone: 6 cm past either end) down the blade, 17 cm.
    Stroke p;
    p.Sample(OnBlade(0.53f, 0.02f, 0.06f));
    EXPECT_TRUE(p.d.Touching());
    p.Move(OnBlade(0.53f, 0.02f, 0.06f), OnBlade(0.36f, 0.02f, 0.06f), 0.45);
    EXPECT_TRUE(p.applied);
    // 10 cm past the tip: not on it.
    Stroke q;
    q.Sample(OnBlade(0.60f, 0.0f, 0.03f));
    EXPECT_FALSE(q.d.Touching());
}

TEST(QuickWipe, EasierACrossCutIsRefused) {
    // Straight across the blade at its middle, 30 cm through it in 0.25 s (inside the wider
    // zone most of the way).
    Stroke p;
    p.Move(OnBlade(0.25f, -0.15f, 0.0f), OnBlade(0.25f, 0.15f, 0.0f), 0.25);
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    // At 45 degrees through it: 20 cm along, as much across, in 0.3 s.
    Stroke d;
    d.Move(OnBlade(0.15f, -0.10f, 0.0f), OnBlade(0.35f, 0.10f, 0.0f), 0.3);
    EXPECT_FALSE(d.applied);
    // A slash of the left hand along the blade (48 cm in 50 ms): over before min_stroke_s.
    Stroke s;
    s.Move(OnBlade(0.0f), OnBlade(0.48f), 0.05);
    EXPECT_FALSE(s.applied);
}

TEST(QuickWipe, EasierATapIsRefused) {
    // Laid on the blade and held there a second (a few mm of jitter).
    Stroke p;
    for (int i = 0; i < 60; ++i) {
        p.Sample(OnBlade(0.25f + 0.004f * static_cast<float>(i % 3), 0.003f * (i % 2), 0.05f));
    }
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    EXPECT_LT(p.best_progress, 0.1f);
    // On and off, twice.
    Stroke q;
    q.Sample(Rest);
    q.Sample(OnBlade(0.2f));
    q.Sample(Rest);
    q.Sample(OnBlade(0.22f));
    q.Sample(Rest);
    EXPECT_FALSE(q.applied);
    // Scrubbing 6 cm back and forth for a second.
    Stroke s;
    for (int k = 0; k < 4; ++k) {
        s.Move(OnBlade(0.20f), OnBlade(0.26f), 0.12);
        s.Move(OnBlade(0.26f), OnBlade(0.20f), 0.12);
    }
    EXPECT_FALSE(s.applied);
}

TEST(QuickWipe, EasierASwingPastThePaperIsRefused) {
    // The paper held still 6 cm off the blade's middle; the blade swung sideways through and
    // past it (40 cm in 0.2 s, 2 m/s): across it, and the paper never moved.
    Stroke p;
    const Q::V3 paper = OnBlade(0.25f, 0.0f, 0.06f);
    p.MoveBlade({-0.20f, 0.0f, 0.0f}, {0.20f, 0.0f, 0.0f}, 0.2, paper, paper);
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    // A thrust along the blade's own line past the still paper: 25 cm at about 1 m/s (0.25 s),
    // the paper 5 cm off: along the blade like a wipe, but the blade moved, not the paper.
    Stroke t;
    const Q::V3 held = OnBlade(0.30f, 0.0f, 0.05f);
    t.Sample(held);
    t.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.25f}, 0.25, held, held);
    EXPECT_TRUE(t.touched);
    EXPECT_FALSE(t.applied);
    EXPECT_GE(t.d.BladeMoves(), 1);
    // ... a slow one too (0.5 s), and a pull back toward the hunter.
    Stroke u;
    u.Sample(held);
    u.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.20f}, 0.5, held, held);
    EXPECT_FALSE(u.applied);
    Stroke b;
    b.Sample(held);
    b.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.20f}, 0.4, held, held);
    EXPECT_FALSE(b.applied);
    // The first version (no own_share) took that thrust for a wipe.
    Stroke o;
    o.c = Q::WipeClassic();
    o.Sample(held);
    o.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.25f}, 0.25, held, held);
    EXPECT_TRUE(o.applied);
}

TEST(QuickWipe, EasierThePaperNearTheBladeWhileFighting) {
    // The paper hand loosely near the blade (9 cm), jittering, while the blade swings back and
    // forth across it three times (1.5 Hz, 30 cm each way) and turns: no wipe.
    Stroke p;
    for (int i = 0; i < 120; ++i) {
        const float s = static_cast<float>(i) / 60.0f;
        const float sw = 0.30f * std::sin(6.2831853f * 1.5f * s);
        const float tw = 0.06f * std::sin(6.2831853f * 0.7f * s);
        p.base = Base + Q::V3{sw, 0.0f, 0.0f};
        p.tip = Tip + Q::V3{sw + tw, 0.04f * std::sin(9.0f * s), 0.0f};
        p.Sample(OnBlade(0.22f + 0.02f * std::sin(5.0f * s), 0.01f * std::sin(13.0f * s),
                         0.09f));
    }
    EXPECT_FALSE(p.applied);
}

TEST(QuickWipe, EasierBothHandsMovingStillApplies) {
    // The paper up the blade 13 cm while the blade comes back 4 cm toward the hunter: a 17 cm
    // stroke, 76% of it the paper's own.
    Stroke p;
    const Q::V3 a = OnBlade(0.15f);
    const Q::V3 b = OnBlade(0.28f);
    p.Sample(a);
    p.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.04f}, 0.4, a, b);
    EXPECT_TRUE(p.applied);
    EXPECT_GT(p.own, 0.6f);
    EXPECT_LT(p.own, 0.95f);
}

TEST(QuickWipe, EasierTheFindCueRises) {
    const Q::WipeConfig c = Q::WipeEasier();
    const auto cue = [&](Q::V3 p) { return Q::WipeNear(c, Q::WipeMeasure(p, Base, Tip)); };
    EXPECT_FLOAT_EQ(cue(OnBlade(0.25f, 0.0f, 0.45f)), 0.0f);      // 33 cm outside the zone
    EXPECT_NEAR(cue(OnBlade(0.25f, 0.0f, 0.27f)), 0.5f, 1e-4f);   // 15 cm outside
    EXPECT_NEAR(cue(OnBlade(0.25f, 0.0f, 0.12f)), 1.0f, 1e-4f);   // at its edge
    EXPECT_FLOAT_EQ(cue(OnBlade(0.25f, 0.0f, 0.05f)), 1.0f);      // in it
    // Rising as it nears, beside the blade past its tip.
    float last = -1.0f;
    for (float d = 0.40f; d >= 0.12f; d -= 0.02f) {
        const float n = cue(OnBlade(0.70f, -d, 0.0f));
        EXPECT_GE(n, last);
        last = n;
    }
    // Off (quick_wipe_cue=0: cue_m 0), no blade, a blade too short: none.
    Q::WipeConfig off = Q::WipeEasier();
    off.cue_m = 0.0f;
    EXPECT_FLOAT_EQ(Q::WipeNear(off, Q::WipeMeasure(OnBlade(0.25f, 0.0f, 0.2f), Base, Tip)), 0.0f);
    EXPECT_FLOAT_EQ(Q::WipeNear(c, Q::WipeMeasure(Base, Base, Base)), 0.0f);
    EXPECT_FLOAT_EQ(Q::WipeNear(c, Q::WipeMeasure(Base, Base, Base + Q::V3{0, 0, -0.08f})), 0.0f);
}

TEST(QuickWipe, EasierControllerCueThenRubApplies) {
    // The defaults in the controller: the paper taken, brought toward the blade (the find cue
    // rising before the touch, none while touching), a 16 cm rub 9 cm off the axis: applied.
    Rig r(2, true, Easier);
    r.Take2(1); // Fire Paper
    r.Step(10);
    EXPECT_FLOAT_EQ(r.out.wipe_near, 0.0f); // 45 cm away: far
    r.Wipe(Rest, OnBlade(0.05f, 0.0f, 0.50f), 0.5); // over the blade, 50 cm up
    EXPECT_FLOAT_EQ(r.out.wipe_near, 0.0f);
    r.nears.clear();
    r.Wipe(OnBlade(0.05f, 0.0f, 0.50f), OnBlade(0.05f, 0.0f, 0.13f), 0.6); // straight down to it
    EXPECT_FALSE(r.touched);
    EXPECT_GT(r.out.wipe_near, 0.9f);
    bool rising = true;
    for (std::size_t i = 1; i < r.nears.size(); ++i) {
        rising &= r.nears[i] + 1e-4f >= r.nears[i - 1];
    }
    EXPECT_TRUE(rising);
    EXPECT_NE(r.log.find("WIPE cue: the paper nears the blade"), std::string::npos);
    r.Wipe(OnBlade(0.05f, 0.0f, 0.13f), OnBlade(0.06f, 0.0f, 0.09f), 0.2);
    r.Step(6);
    EXPECT_TRUE(r.out.wipe_touch);
    EXPECT_FLOAT_EQ(r.out.wipe_near, 0.0f); // the rasp's now
    r.Wipe(OnBlade(0.06f, 0.0f, 0.09f), OnBlade(0.22f, 0.0f, 0.09f), 0.6);
    EXPECT_FALSE(r.near_touch);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 1);
    EXPECT_NE(r.log.find("the paper's own move"), std::string::npos);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.ignite);
    // The cue off (quick_wipe_cue=0): no wipe_near, the rub still applies.
    Rig q(2, true, Easier);
    q.cfg.wipe_cfg.cue_m = 0.0f;
    q.Take2(1);
    q.Step(10);
    q.Wipe(Rest, OnBlade(0.05f, 0.0f, 0.13f), 0.5);
    EXPECT_FLOAT_EQ(q.near_max, 0.0f);
    q.Wipe(OnBlade(0.05f, 0.0f, 0.13f), OnBlade(0.30f, 0.0f, 0.09f), 0.7);
    EXPECT_EQ(q.uses, 1);
}


// ---- The rub (the user's 2026-10-08 "I want it to trigger when I RUB the blade"; the defaults) ---

// The paper's card (8 cm square) at `p`, lying flat over the blade facing up (the eye above it),
// its fist 6 cm nearer the hunter and 2 cm lower.
Q::WipeContact Card(Q::V3 p, Q::V3 base, Q::V3 tip) {
    return Q::WipeMeasureCard(p + Q::V3{0.0f, -0.02f, 0.06f}, p, {1.0f, 0.0f, 0.0f},
                              {0.0f, 0.0f, -1.0f}, 0.04f, 0.04f, base, tip);
}

// The detector with the defaults (the rub) on the card, 60 samples a second, a blade that can
// move.
struct Rubber {
    Q::WipeDetector d;
    Q::WipeConfig c{};
    double t = 10.0;
    double start = 10.0; // the rub's first move
    bool applied = false;
    double applied_t = -1.0;
    int kind = 0;
    float total = 0.0f;
    int strokes = 0;
    bool touched = false;
    double touch_t = -1.0;
    float best_progress = 0.0f;
    int teeth = 0;
    int refused = 0;
    Q::RubWhy why = Q::RubWhy::None;
    Q::V3 base = Base;
    Q::V3 tip = Tip;
    explicit Rubber(double held_for = 1.0) {
        d.Reset(t - held_for);
    }
    void Sample(Q::V3 p) {
        t += 1.0 / 60.0;
        const bool done = d.Update(c, t, Card(p, base, tip), true);
        if (d.Touching() && touch_t < 0.0) {
            touch_t = t;
        }
        touched |= d.Touching();
        best_progress = std::max(best_progress, d.Progress());
        teeth += d.TakeTeeth();
        if (d.Refused() > refused) {
            refused = d.Refused();
            why = d.LastWhy();
        }
        if (done && !applied) {
            applied = true;
            applied_t = t;
            kind = d.DoneKind();
            total = d.RubTotal();
            strokes = d.RubStrokes();
            d.Consume();
        }
    }
    void Move(Q::V3 a, Q::V3 b, double seconds) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds * 60.0)));
        for (int i = 0; i <= n; ++i) {
            Sample(a + (b - a) * static_cast<float>(Stroke::Ease(i, n)));
        }
    }
    // `strokes` one-way strokes between `a` and `b` (from `a`), `seconds` each.
    void Rub(Q::V3 a, Q::V3 b, int strokes_n, double seconds) {
        start = t;
        for (int k = 0; k < strokes_n && !applied; ++k) {
            Move(k % 2 == 0 ? a : b, k % 2 == 0 ? b : a, seconds);
        }
    }
    // The blade shifted by `from` -> `to` over `seconds`, the paper from `p` to `p_to`.
    void MoveBlade(Q::V3 from, Q::V3 to, double seconds, Q::V3 p, Q::V3 p_to) {
        const int n = std::max(1, static_cast<int>(std::lround(seconds * 60.0)));
        for (int i = 0; i <= n; ++i) {
            const float q = static_cast<float>(Stroke::Ease(i, n));
            const Q::V3 shift = from + (to - from) * q;
            base = Base + shift;
            tip = Tip + shift;
            Sample(p + (p_to - p) * q);
        }
    }
    [[nodiscard]] double RubbedFor() const {
        return applied_t - start;
    }
};

TEST(QuickWipe, RubDefaults) {
    const Q::WipeConfig c{};
    EXPECT_TRUE(c.rub);
    EXPECT_FLOAT_EQ(c.blade_m, 0.04f);
    EXPECT_FLOAT_EQ(c.touch_m, 0.03f);
    EXPECT_FLOAT_EQ(c.touch_exit_m, 0.055f);
    EXPECT_FLOAT_EQ(c.rub_m, 0.18f);
    EXPECT_FLOAT_EQ(c.rub_window_s, 1.5f);
    EXPECT_FLOAT_EQ(c.stroke_m, 0.04f);
    EXPECT_FLOAT_EQ(c.blade_max_mps, 0.8f);
    EXPECT_FLOAT_EQ(c.rub_min_s, 0.25f);
    // The one long stroke: the morning's sizes (30% of the blade, 7 .. 20 cm), its own and turn.
    EXPECT_FLOAT_EQ(c.share, 0.30f);
    EXPECT_FLOAT_EQ(c.min_travel_m, 0.07f);
    EXPECT_FLOAT_EQ(c.max_travel_m, 0.20f);
    EXPECT_FLOAT_EQ(c.own_share, 0.40f);
    EXPECT_FLOAT_EQ(c.max_turn_deg, 35.0f);
}

TEST(QuickWipe, RubTheCardOnTheBlade) {
    // The card's nearest point from the blade's axis segment (SegmentRectDistance).
    const Q::V3 x{1.0f, 0.0f, 0.0f};
    const Q::V3 z{0.0f, 0.0f, -1.0f};
    // Flat over the blade 3 cm up: 3 cm; the axis through it: 0.
    EXPECT_NEAR(Card(OnBlade(0.25f, 0.0f, 0.03f), Base, Tip).gap, 0.03f, 1e-4f);
    EXPECT_NEAR(Q::SegmentRectDistance(Base, Tip, OnBlade(0.25f, 0.0f, 0.0f), x,
                                       {0.0f, 1.0f, 0.0f}, 0.04f, 0.04f),
                0.0f, 1e-5f);
    // Beside the blade: 15 cm from its axis, its near edge 11.
    EXPECT_NEAR(Card(OnBlade(0.25f, 0.15f, 0.0f), Base, Tip).gap, 0.11f, 1e-4f);
    // Past the tip: from the tip (8 cm past it, the card's edge 4 cm).
    EXPECT_NEAR(Card(OnBlade(0.58f, 0.0f, 0.0f), Base, Tip).gap, 0.04f, 1e-4f);
    // Upright beside it (its plane along the blade), 6 cm off: its face 6 cm away.
    EXPECT_NEAR(Q::SegmentRectDistance(Base, Tip, OnBlade(0.25f, 0.06f, 0.0f), z,
                                       {0.0f, 1.0f, 0.0f}, 0.04f, 0.04f),
                0.06f, 1e-4f);
    // ... and edge-on to it (its plane across the blade's line, beside it): 6 - 4 = 2 cm.
    EXPECT_NEAR(Q::SegmentRectDistance(Base, Tip, OnBlade(0.25f, 0.06f, 0.0f), x,
                                       {0.0f, 1.0f, 0.0f}, 0.04f, 0.04f),
                0.02f, 1e-4f);
    // The card's centre and the fist both measured; on the blade by the card, not the fist.
    const Q::WipeContact w = Card(OnBlade(0.25f, 0.0f, 0.03f), Base, Tip);
    EXPECT_TRUE(w.card);
    EXPECT_NEAR(w.card_along, 0.25f, 1e-4f);
    EXPECT_NEAR(w.along, 0.19f, 1e-4f); // the fist 6 cm nearer the hunter
    const Q::WipeConfig c{};
    EXPECT_NEAR(Q::WipeSurface(c, w), -0.01f, 1e-4f); // 1 cm into the blade's 4 cm round it
    EXPECT_TRUE(Q::WipeInZone(c, w));
    EXPECT_FALSE(Q::WipeInZone(c, Card(OnBlade(0.25f, 0.0f, 0.08f), Base, Tip))); // 4 cm off
}

TEST(QuickWipe, RubBackAndForthApplies) {
    // 3 strokes of 8 cm back and forth on the flat (0.2 s each, 0.4 m/s): 24 cm, past 18 within
    // the third stroke, about half a second in. The first version and the morning's never.
    Rubber r;
    r.Sample(OnBlade(0.20f, 0.0f, 0.03f));
    r.Rub(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.28f, 0.0f, 0.03f), 3, 0.2);
    EXPECT_TRUE(r.applied);
    EXPECT_EQ(r.kind, 2);
    EXPECT_GE(r.total, 0.18f - 1e-4f);
    EXPECT_EQ(r.strokes, 3);
    EXPECT_LT(r.RubbedFor(), 0.7);
    EXPECT_GT(r.teeth, 4); // the rasp all along
    // Two strokes (16 cm): not yet.
    Rubber two;
    two.Sample(OnBlade(0.20f, 0.0f, 0.03f));
    two.Rub(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.28f, 0.0f, 0.03f), 2, 0.2);
    EXPECT_FALSE(two.applied);
    EXPECT_NEAR(two.best_progress, 16.0f / 18.0f, 0.03f);
    // Four short 5 cm strokes (20 cm): applied.
    Rubber four;
    four.Rub(OnBlade(0.30f, 0.0f, 0.03f), OnBlade(0.35f, 0.0f, 0.03f), 4, 0.15);
    EXPECT_TRUE(four.applied);
    // Strokes of 3 cm (under 4): a scrub, never.
    Rubber scrub;
    scrub.Rub(OnBlade(0.30f, 0.0f, 0.03f), OnBlade(0.33f, 0.0f, 0.03f), 16, 0.1);
    EXPECT_TRUE(scrub.touched);
    EXPECT_FALSE(scrub.applied);
}

TEST(QuickWipe, RubOneLongWipeApplies) {
    // One 15 cm wipe (30% of the 50 cm blade) in 0.35 s, either way: at once.
    Rubber up;
    up.Move(OnBlade(0.05f, 0.0f, 0.03f), OnBlade(0.21f, 0.0f, 0.03f), 0.35);
    EXPECT_TRUE(up.applied);
    EXPECT_EQ(up.kind, 1);
    Rubber down;
    down.Move(OnBlade(0.45f, 0.0f, 0.03f), OnBlade(0.29f, 0.0f, 0.03f), 0.35);
    EXPECT_TRUE(down.applied);
    EXPECT_EQ(down.kind, 1);
    // 12 cm one way: not yet.
    Rubber short_one;
    short_one.Move(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.32f, 0.0f, 0.03f), 0.3);
    EXPECT_FALSE(short_one.applied);
}

TEST(QuickWipe, RubSlowApplies) {
    // A slow, careful rub: 8 cm strokes at 0.15 m/s (0.53 s each) back and forth.
    Rubber r;
    r.Rub(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.28f, 0.0f, 0.03f), 4, 0.08 / 0.15);
    EXPECT_TRUE(r.applied);
    EXPECT_LT(r.RubbedFor(), 1.45);
    // ... and one slow 16 cm wipe at 0.15 m/s.
    Rubber s;
    s.Move(OnBlade(0.10f, 0.0f, 0.03f), OnBlade(0.26f, 0.0f, 0.03f), 0.16 / 0.15);
    EXPECT_TRUE(s.applied);
    // Too slow: 6 cm strokes at 0.04 m/s (1.5 s each) never add up within 1.5 s.
    Rubber z;
    z.Rub(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.26f, 0.0f, 0.03f), 4, 1.5);
    EXPECT_FALSE(z.applied);
}

TEST(QuickWipe, RubBothWaysTheSame) {
    // Base -> tip and tip -> base, one long wipe and the back and forth, at the guard and at the
    // tip: applied after the same time to the sample.
    const auto one = [](float from, float to, double s) {
        Rubber r;
        r.Sample(OnBlade(from, 0.0f, 0.03f));
        r.start = r.t;
        r.Move(OnBlade(from, 0.0f, 0.03f), OnBlade(to, 0.0f, 0.03f), s);
        return r.applied ? r.RubbedFor() : -1.0;
    };
    for (const double s : {0.25, 0.5, 1.0}) {
        const double up = one(0.02f, 0.20f, s);
        const double down = one(0.48f, 0.30f, s);
        const double down_guard = one(0.20f, 0.02f, s);
        EXPECT_GT(up, 0.0);
        EXPECT_NEAR(up, down, 1.0 / 60.0 + 1e-6);
        EXPECT_NEAR(up, down_guard, 1.0 / 60.0 + 1e-6);
    }
    const auto rub = [](float a, float b) {
        Rubber r;
        r.Sample(OnBlade(a, 0.0f, 0.03f));
        r.Rub(OnBlade(a, 0.0f, 0.03f), OnBlade(b, 0.0f, 0.03f), 4, 0.25);
        return r.applied ? r.RubbedFor() : -1.0;
    };
    const double guard_up = rub(0.02f, 0.10f);
    const double tip_down = rub(0.50f, 0.42f);
    EXPECT_GT(guard_up, 0.0);
    EXPECT_NEAR(guard_up, tip_down, 1.0 / 60.0 + 1e-6);
}

TEST(QuickWipe, RubOnTheFlatTheEdgeTheGuardTheTip) {
    struct Where {
        float a, b, x, y;
    };
    // The flat (3 cm over the axis), the edge (the card beside the blade, 6 cm off its axis),
    // tilted over the edge (4 cm out, 4 up), the guard (from the base), the tip (to 4 cm past it).
    for (const Where w : {Where{0.20f, 0.28f, 0.0f, 0.03f}, Where{0.20f, 0.28f, 0.06f, 0.0f},
                          Where{0.20f, 0.28f, -0.04f, 0.04f}, Where{0.0f, 0.08f, 0.0f, 0.03f},
                          Where{0.46f, 0.54f, 0.0f, 0.03f}}) {
        Rubber r;
        r.Rub(OnBlade(w.a, w.x, w.y), OnBlade(w.b, w.x, w.y), 4, 0.2);
        EXPECT_TRUE(r.applied);
        EXPECT_LT(r.RubbedFor(), 0.8);
    }
}

TEST(QuickWipe, RubPastTheEndsIsAlong) {
    // The merge skeptic's find: short strokes at the guard or the tip with the card's centre past
    // the blade's base or tip (within the end margin) were refused as "across" (the card's offset
    // from the end point there also runs along the blade). Now they count like any other.
    struct Where {
        float a, b;
    };
    for (const Where w : {Where{-0.055f, -0.005f}, Where{-0.05f, 0.0f}, Where{0.50f, 0.555f},
                          Where{0.505f, 0.555f}}) {
        Rubber r;
        r.Rub(OnBlade(w.a, 0.0f, 0.03f), OnBlade(w.b, 0.0f, 0.03f), 6, 0.2);
        EXPECT_TRUE(r.applied);
        EXPECT_EQ(r.refused, 0);
        EXPECT_LT(r.RubbedFor(), 1.0);
    }
    // ... while a cut across the blade there is still across.
    Rubber x;
    x.Rub(OnBlade(-0.03f, -0.12f, 0.03f), OnBlade(-0.03f, 0.12f, 0.03f), 4, 0.25);
    EXPECT_FALSE(x.applied);
}

TEST(QuickWipe, RubHeldStillNever) {
    // Laid on the blade and held there 3 s, a few mm of jitter and a slow 1 cm/s drift.
    Rubber r;
    for (int i = 0; i < 180; ++i) {
        const float j = 0.003f * std::sin(static_cast<float>(i) * 1.7f);
        r.Sample(OnBlade(0.25f + j + 0.01f * static_cast<float>(i) / 60.0f, 0.002f * (i % 2),
                         0.03f));
    }
    EXPECT_TRUE(r.touched);
    EXPECT_FALSE(r.applied);
    EXPECT_LT(r.best_progress, 0.25f);
}

TEST(QuickWipe, RubSwingPastNever) {
    // The paper held still on the blade's middle; the blade swung sideways through it (40 cm in
    // 0.2 s); thrust along its own line past it (25 cm at 1 m/s, 20 cm at 0.4 m/s), pulled back;
    // and swung back and forth across it for 2 s while it is held by it, jittering.
    Rubber p;
    const Q::V3 paper = OnBlade(0.25f, 0.0f, 0.03f);
    p.MoveBlade({-0.20f, 0.0f, 0.0f}, {0.20f, 0.0f, 0.0f}, 0.2, paper, paper);
    EXPECT_TRUE(p.touched);
    EXPECT_FALSE(p.applied);
    Rubber t;
    t.Sample(paper);
    t.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.25f}, 0.25, paper, paper);
    EXPECT_TRUE(t.touched);
    EXPECT_FALSE(t.applied);
    EXPECT_GE(t.d.BladeMoves(), 1);
    EXPECT_TRUE(t.why == Q::RubWhy::BladeMoving);
    Rubber u;
    u.Sample(paper);
    u.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.20f}, 0.5, paper, paper);
    u.Move(paper, paper + Q::V3{0.0f, 0.20f, 0.0f}, 0.2); // lifted off: the stroke judged
    EXPECT_FALSE(u.applied);
    EXPECT_TRUE(u.why == Q::RubWhy::NotOwn);
    Rubber b;
    b.Sample(paper);
    b.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.20f}, 0.4, paper, paper);
    EXPECT_FALSE(b.applied);
    // Thrust in and out along it three times (a stabbing rhythm, 12 cm at 0.5 m/s): never.
    Rubber s;
    s.Sample(paper);
    for (int k = 0; k < 3; ++k) {
        s.MoveBlade({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.12f}, 0.24, paper, paper);
        s.MoveBlade({0.0f, 0.0f, -0.12f}, {0.0f, 0.0f, 0.0f}, 0.24, paper, paper);
    }
    EXPECT_FALSE(s.applied);
    Rubber f;
    for (int i = 0; i < 120; ++i) {
        const float sec = static_cast<float>(i) / 60.0f;
        const float sw = 0.30f * std::sin(6.2831853f * 1.5f * sec);
        const float tw = 0.06f * std::sin(6.2831853f * 0.7f * sec);
        f.base = Base + Q::V3{sw, 0.0f, 0.0f};
        f.tip = Tip + Q::V3{sw + tw, 0.04f * std::sin(9.0f * sec), 0.0f};
        f.Sample(OnBlade(0.22f + 0.02f * std::sin(5.0f * sec), 0.01f * std::sin(13.0f * sec),
                         0.03f));
    }
    EXPECT_FALSE(f.applied);
    // A forehand with the paper held by the hilt, the torso turning with it.
    const Q::WipeConfig c{};
    for (const float side : {0.04f, 0.06f, 0.12f}) {
        EXPECT_FALSE(SwingWithThePaperByTheHilt(c, 110.0f, 30.0f, 0.25, side, 0.10f));
    }
    for (const float deg : {40.0f, 60.0f, 90.0f}) {
        for (const double sec : {0.25, 0.5}) {
            EXPECT_FALSE(SwingWithThePaperByTheHilt(c, deg, deg / 3.0f, sec, 0.05f, 0.10f));
        }
    }
}

TEST(QuickWipe, RubBesideTheBladeNever) {
    // Rubbed back and forth 15 cm beside the blade, 9 cm over it (the card's face 5 cm off its
    // surface) and past its tip: never on it, nothing.
    for (const Q::V3 off : {Q::V3{0.15f, 0.0f, 0.0f}, Q::V3{-0.15f, 0.0f, 0.0f},
                            Q::V3{0.0f, 0.09f, 0.0f}, Q::V3{0.12f, 0.06f, 0.0f}}) {
        Rubber r;
        r.Rub(OnBlade(0.20f, off.x, off.y), OnBlade(0.30f, off.x, off.y), 8, 0.2);
        EXPECT_FALSE(r.touched);
        EXPECT_FALSE(r.applied);
    }
    Rubber tip;
    tip.Rub(OnBlade(0.62f, 0.0f, 0.03f), OnBlade(0.72f, 0.0f, 0.03f), 8, 0.2);
    EXPECT_FALSE(tip.touched);
    EXPECT_FALSE(tip.applied);
}

TEST(QuickWipe, RubCrossCutNever) {
    // Straight across the blade (30 cm through it in 0.25 s), back and forth four times.
    Rubber r;
    r.Rub(OnBlade(0.25f, -0.15f, 0.03f), OnBlade(0.25f, 0.15f, 0.03f), 4, 0.25);
    EXPECT_TRUE(r.touched);
    EXPECT_FALSE(r.applied);
    // At 45 degrees, back and forth (14 cm along, as much across), four times.
    Rubber d;
    d.Rub(OnBlade(0.18f, -0.07f, 0.03f), OnBlade(0.32f, 0.07f, 0.03f), 4, 0.3);
    EXPECT_FALSE(d.applied);
    EXPECT_TRUE(d.why == Q::RubWhy::Across);
    // ... and a short one, 5 cm along and across, eight times.
    Rubber e;
    e.Rub(OnBlade(0.22f, -0.025f, 0.03f), OnBlade(0.27f, 0.025f, 0.03f), 8, 0.15);
    EXPECT_FALSE(e.applied);
    // While a rub a little crooked (8 cm along, 3 across: 20 degrees) applies.
    Rubber k;
    k.Rub(OnBlade(0.20f, -0.015f, 0.03f), OnBlade(0.28f, 0.015f, 0.03f), 4, 0.2);
    EXPECT_TRUE(k.applied);
    // A slash of the left hand along it (48 cm in 50 ms).
    Rubber s;
    s.Move(OnBlade(0.0f, 0.0f, 0.03f), OnBlade(0.48f, 0.0f, 0.03f), 0.05);
    EXPECT_FALSE(s.applied);
}

TEST(QuickWipe, RubPokeNever) {
    // Poked down onto the blade and lifted off, five times (0.1 s down, 0.1 s up, each a little
    // further along), and touched on and off.
    Rubber r;
    for (int k = 0; k < 5; ++k) {
        const float a = 0.20f + 0.02f * static_cast<float>(k);
        r.Move(OnBlade(a, 0.0f, 0.15f), OnBlade(a + 0.01f, 0.0f, 0.02f), 0.1);
        r.Move(OnBlade(a + 0.01f, 0.0f, 0.02f), OnBlade(a + 0.02f, 0.0f, 0.15f), 0.1);
    }
    EXPECT_TRUE(r.touched);
    EXPECT_FALSE(r.applied);
    // Stabbed at the blade sideways (the card's edge into the edge) and back, five times.
    Rubber s;
    for (int k = 0; k < 5; ++k) {
        s.Move(OnBlade(0.25f, 0.20f, 0.0f), OnBlade(0.26f, 0.03f, 0.0f), 0.12);
        s.Move(OnBlade(0.26f, 0.03f, 0.0f), OnBlade(0.25f, 0.20f, 0.0f), 0.12);
    }
    EXPECT_FALSE(s.applied);
}

TEST(QuickWipe, RubKeepsItsStrokesThroughALift) {
    // Two strokes, the card lifted off for 0.2 s at the turn, one more: the rub goes on.
    Rubber r;
    r.Rub(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.28f, 0.0f, 0.03f), 2, 0.2);
    EXPECT_FALSE(r.applied);
    r.Move(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.20f, 0.0f, 0.12f), 0.1);
    EXPECT_FALSE(r.d.Touching());
    r.Move(OnBlade(0.20f, 0.0f, 0.12f), OnBlade(0.20f, 0.0f, 0.03f), 0.1);
    r.Move(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.28f, 0.0f, 0.03f), 0.2);
    EXPECT_TRUE(r.applied);
    EXPECT_EQ(r.kind, 2);
}

TEST(QuickWipe, RubBothHandsMovingApplies) {
    // The paper back and forth 7 cm while the blade comes 2 cm toward it each time (both hands
    // working, the blade slow): applied.
    Rubber r;
    const Q::V3 a = OnBlade(0.20f, 0.0f, 0.03f);
    const Q::V3 b = OnBlade(0.27f, 0.0f, 0.03f);
    r.start = r.t;
    for (int k = 0; k < 4 && !r.applied; ++k) {
        const bool out = k % 2 == 0;
        r.MoveBlade(out ? Q::V3{} : Q::V3{0.0f, 0.0f, 0.02f}, out ? Q::V3{0.0f, 0.0f, 0.02f} : Q::V3{},
                    0.25, out ? a : b, out ? b : a);
    }
    EXPECT_TRUE(r.applied);
}

TEST(QuickWipe, RubFindCue) {
    // The cue from the card's gap to the blade's surface: 1 at touch_m, 0 from cue_m past it.
    const Q::WipeConfig c{};
    const auto cue = [&](Q::V3 p) { return Q::WipeNear(c, Card(p, Base, Tip)); };
    EXPECT_FLOAT_EQ(cue(OnBlade(0.25f, 0.0f, 0.45f)), 0.0f);     // its face 41 cm off
    EXPECT_NEAR(cue(OnBlade(0.25f, 0.0f, 0.22f)), 0.5f, 1e-3f);  // 18 off: 15 outside
    EXPECT_NEAR(cue(OnBlade(0.25f, 0.0f, 0.07f)), 1.0f, 1e-3f);  // at the zone's edge
    EXPECT_FLOAT_EQ(cue(OnBlade(0.25f, 0.0f, 0.03f)), 1.0f);     // on it
}

TEST(QuickWipe, RubController) {
    // The controller with the defaults: taken, held near the blade's surface (the near-miss log),
    // laid on it (the touch-down at once, the log), a cut across (not counted, logged), lifted
    // (contact lost, logged), then rubbed back and forth: applied, the rub in the log.
    Rig r(2, true, RubMode);
    r.Take2(1); // Fire Paper
    r.Step(10);
    r.Wipe(Rest, OnBlade(0.20f, 0.0f, 0.10f), 0.5); // over the blade, its face 6 cm off it
    r.Step(70);
    EXPECT_FALSE(r.touched);
    EXPECT_NE(r.log.find("rub: near the blade, not on it"), std::string::npos);
    EXPECT_NE(r.log.find("on within 3"), std::string::npos);
    r.Wipe(OnBlade(0.20f, 0.0f, 0.10f), OnBlade(0.20f, 0.0f, 0.03f), 0.15);
    r.Step(4);
    EXPECT_TRUE(r.out.wipe_touch);
    EXPECT_NE(r.log.find("WIPE the paper on the blade (its card"), std::string::npos);
    // Across it twice (back and forth): no use, a stroke not counted.
    r.Wipe(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.21f, 0.06f, 0.03f), 0.15);
    r.Wipe(OnBlade(0.21f, 0.06f, 0.03f), OnBlade(0.20f, -0.06f, 0.03f), 0.25);
    r.Wipe(OnBlade(0.20f, -0.06f, 0.03f), OnBlade(0.20f, 0.0f, 0.03f), 0.15);
    EXPECT_EQ(r.uses, 0);
    // Lifted off.
    r.Wipe(OnBlade(0.20f, 0.0f, 0.03f), OnBlade(0.20f, 0.0f, 0.20f), 0.2);
    r.Step(6);
    EXPECT_FALSE(r.out.wipe_touch);
    EXPECT_NE(r.log.find("rub: contact lost (lifted: off the blade by"), std::string::npos);
    r.Step(120); // the window passes
    // Back on, three 8 cm strokes.
    r.Wipe(OnBlade(0.20f, 0.0f, 0.20f), OnBlade(0.20f, 0.0f, 0.03f), 0.2);
    r.Step(4);
    const int teeth0 = r.teeth;
    for (int k = 0; k < 3 && r.uses == 0; ++k) {
        r.Wipe(OnBlade(k % 2 == 0 ? 0.20f : 0.28f, 0.0f, 0.03f),
               OnBlade(k % 2 == 0 ? 0.28f : 0.20f, 0.0f, 0.03f), 0.2);
    }
    r.Step(6);
    EXPECT_GT(r.teeth, teeth0 + 3);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.wipe_uses, 1);
    EXPECT_NE(r.log.find("wiped along the blade: rubbed"), std::string::npos);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.ignite);
    // A stroke not counted shows in the log (a cut across, once more, in a fresh take).
    Rig q(2, true, RubMode);
    q.Take2(1);
    q.Step(10);
    q.Wipe(Rest, OnBlade(0.25f, -0.15f, 0.03f), 0.4);
    for (int k = 0; k < 3; ++k) {
        q.Wipe(OnBlade(0.18f, -0.08f, 0.03f), OnBlade(0.32f, 0.08f, 0.03f), 0.3);
        q.Wipe(OnBlade(0.32f, 0.08f, 0.03f), OnBlade(0.18f, -0.08f, 0.03f), 0.3);
    }
    EXPECT_EQ(q.uses, 0);
    EXPECT_NE(q.log.find("rub: a stroke not counted"), std::string::npos);
    EXPECT_NE(q.log.find("across the blade"), std::string::npos);
}

TEST(QuickWipe, RubTouchDownAtOnce) {
    // The touch-down: the first sample with the card within touch_m of the surface is the frame
    // the rasp starts (no wait for movement).
    Rig r(2, true, RubMode);
    r.Take2(1);
    r.Step(10);
    r.paper = OnBlade(0.25f, 0.0f, 0.12f);
    r.Step(12);
    EXPECT_FALSE(r.out.wipe_touch);
    r.paper = OnBlade(0.25f, 0.0f, 0.069f); // its face 2.9 cm off the blade's surface
    int frames = 0;
    while (!r.out.wipe_touch && frames < 10) {
        r.Step();
        ++frames;
    }
    EXPECT_TRUE(r.out.wipe_touch);
    EXPECT_LE(frames, SampleEvery); // the next game sample
}

TEST(QuickWipe, CueSettingIsOnOff) {
    // quick_wipe_cue (2026-10-09: the blade's light gone at the user's request, "REMOVE THAT
    // GLOW"): on / off for the ticks on the paper hand, on by default; an older vr.ini's 2 (the
    // light only) and 3 (both) read as on, 0 and false as off.
    Q::QuickSettings& s = Q::GetQuickSettings();
    const Q::QuickSettings saved = s;
    EXPECT_TRUE(Q::QuickSettings{}.wipe_cue);
    const struct {
        const char* value;
        bool on;
    } cases[] = {{"0", false},     {"1", true},    {"2", true},    {"3", true},
                 {"false", false}, {"true", true}, {"0.0", false}, {"2.0", true}};
    for (const auto& c : cases) {
        s.wipe_cue = !c.on;
        EXPECT_TRUE(Q::ParseQuickSetting("quick_wipe_cue", c.value));
        EXPECT_EQ(s.wipe_cue, c.on);
    }
    s = saved;
}


// Curved whip: the paper must touch real links, including across their joints.
std::array<Q::V3, 31> CurvedWipeChain() {
    std::array<Q::V3, 31> p{};
    for (int i = 0; i < 31; ++i) {
        const float a = i * .03f / .6f;
        p[i] = Base + Q::V3{.6f * (1 - std::cos(a)), -.6f * std::sin(a), 0};
    }
    return p;
}
Q::V3 CurvePoint(float u) {
    return Base + Q::V3{.6f * (1 - std::cos(u / .6f)), -.6f * std::sin(u / .6f), .025f};
}
template <std::size_t N>
Q::WipeContact ChainCard(Q::V3 p, const std::array<Q::V3, N>& chain) {
    return Q::WipeMeasurePath(p, p, {1,0,0}, {0,1,0}, .04f, .04f,
                              chain.data(), static_cast<int>(N));
}

TEST(QuickWipe, WhipRubAcrossJointsAwayFromHilt) {
    const auto chain = CurvedWipeChain();
    Q::WipeDetector d;
    d.Reset(9);
    bool used = false;
    int last_edge = -1, joints = 0;
    for (int i = 0; i <= 72; ++i) {
        const float u = .34f + .09f * std::cos(i * 3.14159265f / 18);
        const auto w = ChainCard(CurvePoint(u), chain);
        ASSERT_TRUE(w.valid);
        EXPECT_GT(w.length, .89f);
        if (last_edge >= 0 && last_edge != w.path_edge) ++joints;
        last_edge = w.path_edge;
        used |= d.Update({}, 10 + i / 60.0, w, true);
        if (used) break;
    }
    EXPECT_TRUE(used);
    EXPECT_GT(joints, 3);
    EXPECT_EQ(d.BladeMoves(), 0); // changing tangent is not the whip swinging
}

TEST(QuickWipe, WhipContactUsesPaperEdgeNotJustItsCentre) {
    const std::array<Q::V3, 4> chain{{{-.02f,.09f,0},{.02f,.09f,0},
                                      {.12f,.3f,0},{.12f,-.05f,0}}};
    const auto w = Q::WipeMeasurePath({}, {}, {1,0,0}, {0,0,1}, .08f, .04f,
                                      chain.data(), static_cast<int>(chain.size()));
    ASSERT_TRUE(w.valid);
    EXPECT_EQ(w.path_edge, 2);
    EXPECT_NEAR(w.gap, .04f, 1e-5f);
    EXPECT_TRUE(Q::WipeInZone({},w));
}

TEST(QuickWipe, WhipNoInvisibleChordAcrossCurve) {
    const std::array<Q::V3, 4> chain{{{0,0,0},{.4f,0,0},{.4f,0,.4f},{0,0,.4f}}};
    const auto w = ChainCard(Q::V3{0,0,.2f}, chain);
    ASSERT_TRUE(w.valid);
    EXPECT_FALSE(Q::WipeInZone({}, w));
    Q::WipeDetector d;
    d.Reset(9);
    for (int i = 0; i < 120; ++i) {
        const auto p = Q::V3{0,0,.2f + .04f * std::sin(i * .2f)};
        EXPECT_FALSE(d.Update({}, 10+i/60.0, ChainCard(p, chain), true));
        EXPECT_FALSE(d.Touching());
    }
}

TEST(QuickWipe, WhipStillPaperAndMovingChainNeverApplies) {
    Q::WipeDetector d;
    d.Reset(9);
    for (int i = 0; i < 180; ++i) {
        auto chain = CurvedWipeChain();
        for (auto& p : chain) p.y += .12f * std::sin(i * .12f);
        EXPECT_FALSE(d.Update({}, 10+i/60.0, ChainCard(CurvePoint(.34f), chain), true));
    }
}

TEST(QuickWipe, WhipFastMotionAtPaperRejectsEvenIfBothHandsMove) {
    Q::WipeDetector d;
    d.Reset(9);
    bool rejected = false;
    for (int i = 0; i < 90; ++i) {
        auto chain = CurvedWipeChain();
        const Q::V3 motion{0,0,1.2f*i/60.0f};
        for (auto& p : chain) p = p + motion;
        const auto w = ChainCard(CurvePoint(.34f+.08f*std::sin(i*.2f))+motion, chain);
        EXPECT_FALSE(d.Update({}, 10+i/60.0, w, true));
        rejected |= d.LastWhy() == Q::RubWhy::BladeMoving;
    }
    EXPECT_TRUE(rejected);
}

TEST(QuickWipe, WhipContactLostAndReacquiredKeepsOnlyRealStrokes) {
    const auto chain = CurvedWipeChain();
    Q::WipeDetector d;
    d.Reset(9);
    for (int i = 0; i <= 18; ++i)
        EXPECT_FALSE(d.Update({}, 10+i/60.0, ChainCard(CurvePoint(.25f+.08f*i/18), chain), true));
    auto lifted = CurvePoint(.33f); lifted.z += .2f;
    EXPECT_FALSE(d.Update({}, 10.32, ChainCard(lifted, chain), true));
    EXPECT_FALSE(d.Touching());
    bool used = false;
    for (int i = 0; i <= 54; ++i) {
        const auto w = ChainCard(CurvePoint(.29f+.04f*std::cos(i*3.14159265f/18)), chain);
        used |= d.Update({}, 10.4+i/60.0, w, true);
    }
    EXPECT_TRUE(used);
}

TEST(QuickWipe, WhipPathEndsAndHandleExcluded) {
    const std::array<Q::V3, 3> chain{{{0,-.25f,0},{0,-.5f,0},{0,-.75f,0}}};
    EXPECT_FALSE(Q::WipeInZone({}, ChainCard(Q::V3{0,0,.025f}, chain)));
    EXPECT_TRUE(Q::WipeInZone({}, ChainCard(Q::V3{0,-.27f,.025f}, chain)));
    EXPECT_TRUE(Q::WipeInZone({}, ChainCard(Q::V3{0,-.73f,.025f}, chain)));
    Q::WipeDetector d;
    d.Reset(9);
    EXPECT_FALSE(d.Update({}, 10, ChainCard(Q::V3{0,-.85f,.025f}, chain), true));
    EXPECT_FALSE(d.Touching());
}

TEST(QuickWipe, WhipInvalidPathDoesNotFallBackToHandle) {
    const auto check = [](const Q::V3* p, int n) {
        return Q::WipeMeasurePath({}, {}, {1,0,0}, {0,1,0}, .04f, .04f, p, n).valid;
    };
    auto chain = CurvedWipeChain();
    EXPECT_FALSE(check(nullptr, 3));
    EXPECT_FALSE(check(chain.data(), 1));
    EXPECT_FALSE(check(chain.data(), 41));
    chain[3] = chain[2];
    EXPECT_FALSE(check(chain.data(), 31));
    chain = CurvedWipeChain();
    chain.back().x = NAN;
    EXPECT_FALSE(check(chain.data(), 31));
}

TEST(QuickWipe, WhipRubFollowsTighterStationaryBend) {
    std::array<Q::V3, 40> chain{};
    constexpr float radius = .12f;
    for (int j=0; j<40; ++j) {
        const float u=j*.01f;
        chain[j]={radius*std::cos(u/radius),radius*std::sin(u/radius),0};
    }
    Q::WipeDetector d;
    d.Reset(9);
    bool applied=false;
    for (int i=0; i<=144; ++i) {
        const float u=.15f+.06f*std::cos(i*3.14159265f/24);
        const Q::V3 p{radius*std::cos(u/radius),radius*std::sin(u/radius),.025f};
        applied |= d.Update({},10+i/60.0,ChainCard(p,chain),true);
        if(applied) break;
    }
    EXPECT_TRUE(applied);
    EXPECT_EQ(d.BladeMoves(),0);
}

TEST(QuickWipe, WhipRetractingUpstreamDoesNotAddRubTravel) {
    // The contacted blade remains fixed. Changing the chain upstream of it must
    // not turn a 3 cm paper jitter into a qualifying 6 cm rub.
    Q::WipeDetector d;
    d.Reset(9);
    bool applied=false;
    for (int i=0; i<=240; ++i) {
        const float phase=std::cos(i*3.14159265f/12);
        const float shift=.015f*phase;
        const std::array<Q::V3,5> chain{{{0,-.2f+shift,0},{0,shift,0},
                                        {0,.2f,0},{0,.4f,0},{0,.6f,0}}};
        const Q::V3 p{0,.3f-.015f*phase,.025f};
        applied |= d.Update({},10+i/60.0,ChainCard(p,chain),true);
    }
    EXPECT_FALSE(applied);
}

TEST(QuickWipe, WhipActualRotationStillRejectsStroke) {
    Q::WipeDetector d;
    d.Reset(9);
    bool used=false, turned=false;
    for(int i=0;i<=180;++i) {
        const float a=i/60.0f*3.4906585f; // 200 degrees/s, around the paper rather than the hilt
        const float u=.3f+.05f*std::cos(i*3.14159265f/30);
        std::array<Q::V3,31> chain{};
        for(int j=0;j<31;++j) {
            const float x=j*.03f-.3f;
            chain[j]={x*std::cos(a),x*std::sin(a),0};
        }
        const Q::V3 p{(u-.3f)*std::cos(a),(u-.3f)*std::sin(a),.025f};
        used |= d.Update({},10+i/60.0,ChainCard(p,chain),true);
        turned |= d.LastWhy()==Q::RubWhy::BladeTurned;
    }
    EXPECT_FALSE(used);
    EXPECT_TRUE(turned);
}

TEST(QuickWipe, WhipNearbyFoldDoesNotCountJumpBetweenDistantLinks) {
    const std::array<Q::V3,6> chain{{{0,0,0},{.4f,0,0},{.4f,.4f,0},
                                    {0,.4f,0},{0,.06f,0},{.4f,.06f,0}}};
    Q::WipeDetector d;
    d.Reset(9);
    bool used=false;
    int changes=0, last=-1;
    for(int i=0;i<=180;++i) {
        const Q::V3 p{.2f,.03f+.005f*std::cos(i*.4f),.025f};
        const auto w=ChainCard(p,chain);
        if(last>=0 && last!=w.path_edge) ++changes;
        last=w.path_edge;
        used |= d.Update({},10+i/60.0,w,true);
    }
    EXPECT_GT(changes,10);
    EXPECT_FALSE(used);
    EXPECT_LT(d.RubTotal(),.04f);
}

TEST(QuickWipe, WhipRubStraddlingBladeEndCounts) {
    const std::array<Q::V3,4> chain{{{0,0,0},{0,.3f,0},{0,.6f,0},{0,.9f,0}}};
    for(float end:{0.0f,.9f}) {
        Q::WipeDetector d;
        d.Reset(9);
        bool used=false;
        for(int i=0;i<=144;++i) {
            const Q::V3 p{0,end+.03f*std::cos(i*3.14159265f/18),.025f};
            used |= d.Update({},10+i/60.0,ChainCard(p,chain),true);
            if(used) break;
        }
        EXPECT_TRUE(used) << end;
    }
}

TEST(QuickWipe, WhipModeSwitchClearsRubProgress) {
    Q::WipeDetector d;
    d.Reset(9);
    for (int i = 0; i <= 18; ++i) {
        auto p = OnBlade(.2f+.1f*i/18);
        EXPECT_FALSE(d.Update({}, 10+i/60.0,
            Q::WipeMeasureCard(p,p,{1,0,0},{0,0,-1},.04f,.04f,Base,Tip),true));
    }
    EXPECT_GT(d.Progress(), .4f);
    EXPECT_FALSE(d.Update({}, 10.32, ChainCard(CurvePoint(.25f),CurvedWipeChain()),true));
    EXPECT_FLOAT_EQ(d.RubTotal(),0);
}

} // namespace
