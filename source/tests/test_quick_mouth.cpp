// SPDX-License-Identifier: GPL-2.0-or-later

// Eat and drink by bringing the item to the mouth (quick_use_mouth, the user's 2026-10-06
// request): the pellets and antidotes (eaten) and the elixirs and the sedative (drunk) taken from
// the quick-item arc stay in the hunter's hand and are used when the hand brings them to the
// mouth; let go of away from it they go back, nothing used. Throwables never (one brought near
// the face stays in the hand and still throws). core/vr/quick_items.h: MouthOf, MouthPoint,
// MouthGate and the Controller in both left-hand layouts.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"

namespace {

namespace Q = VR::Quick;

constexpr double Dt = 1.0 / 90.0;

// The eyes 1.6 m up looking along -Z: the mouth 8 cm below and 8 cm in front of them.
const Q::V3 Eye{0.0f, 1.6f, 0.0f};
const Q::V3 Ahead{0.0f, 0.0f, -1.0f};
const Q::V3 Mouth = Q::MouthPoint(Eye, Ahead, 0.08f, 0.08f);
// The hand at rest in front of the belly, and on the arc (the cards are 32 cm and more from the
// eyes).
const Q::V3 Rest{-0.2f, 1.1f, -0.3f};

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, -1, true}; // Molotov
    s.slot[1] = {81, 1120, 3, 18, -1, true}; // Blue Elixir (drunk)
    s.slot[2] = {82, 1110, 3, 20, -1, true}; // Beast Blood Pellet (eaten)
    s.slot[3] = {83, 1100, 3, 21, -1, true}; // Antidote (eaten)
    s.slot[4] = {84, 1300, 3, 22, -1, true}; // Fire Paper (rubbed on the blade: at the pull)
    s.slot[5] = {85, 1510, 3, 23, -1, true}; // Coldblood Dew (crushed: at the pull)
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

struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{};  // the last request any step sent
    int uses = 0;       // Use requests sent
    int throws = 0;     // Throw requests sent
    int mouth_uses = 0; // ... of them at the mouth
    std::string log;
    bool gulp = false, bite = false, use_feel = false, deny = false, putback = false;
    double t = 100.0;

    explicit Rig(int scheme = 2, bool mouth = true) {
        cfg.left_scheme = scheme;
        cfg.mouth = mouth;
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = Rest;
        in.mouth_valid = true;
        in.mouth = Mouth;
        in.snapshot_fresh = true;
        in.snapshot = Slots();
    }
    const Q::Output& Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            t += Dt;
            in.t = t;
            in.dt = static_cast<float>(Dt);
            in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
            c.Update(cfg, in, out);
            if (out.send) {
                sent = out.request;
                uses += sent.action == Q::Action::Use ? 1 : 0;
                throws += sent.action == Q::Action::Throw ? 1 : 0;
                mouth_uses += sent.mouth != 0 ? 1 : 0;
            }
            gulp |= Has(out, Q::Haptic::Gulp);
            bite |= Has(out, Q::Haptic::Bite);
            use_feel |= Has(out, Q::Haptic::Use);
            deny |= Has(out, Q::Haptic::Deny);
            putback |= Has(out, Q::Haptic::PutBack);
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
    // The hand to `to` over `seconds`, minimum jerk (its velocity with it), then still.
    void MoveHand(Q::V3 to, double seconds) {
        const Q::V3 from = in.hand_p;
        const int n = std::max(1, static_cast<int>(std::lround(seconds / Dt)));
        for (int i = 1; i <= n; ++i) {
            const double s = static_cast<double>(i) / n;
            const double p = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
            const double v = (30 * s * s - 60 * s * s * s + 30 * s * s * s * s) / seconds;
            in.hand_p = from + (to - from) * static_cast<float>(p);
            in.hand_v = (to - from) * static_cast<float>(v);
            Step();
        }
        in.hand_v = {};
    }
    // left_scheme=2: the grip opens the arc, the hand on `slot` past the dwell, the trigger takes.
    void Take2(int slot) {
        Grip(1.0f);
        Step();
        Grip(0.0f);
        Step();
        Hover(slot);
        Step(20);
        Trigger(1.0f);
        Step();
        in.hover.slot = -1; // the hand leaves the arc
    }
    // left_scheme=1: Y opens, the hand on `slot`, the grip takes (and stays squeezed).
    void Take1(int slot) {
        in.y = true;
        Step();
        Hover(slot);
        Step(20);
        Grip(1.0f);
        Step();
        in.hover.slot = -1;
    }
    void Result(Q::Outcome o) {
        in.have_result = true;
        in.result = {sent.serial, o, "", sent.goods, 3, o == Q::Outcome::Used ? 2 : 3};
        Step();
        in.have_result = false;
    }
};

// ---- What goes to the mouth -----------------------------------------------------------------

TEST(QuickMouth, Classification) {
    EXPECT_TRUE(Q::MouthOf(1110) == Q::MouthKind::Eat);   // Beast Blood Pellet
    EXPECT_TRUE(Q::MouthOf(1100) == Q::MouthKind::Eat);   // Antidote
    EXPECT_TRUE(Q::MouthOf(1101) == Q::MouthKind::Drink); // Sedative
    EXPECT_TRUE(Q::MouthOf(1120) == Q::MouthKind::Drink); // Blue Elixir
    EXPECT_TRUE(Q::MouthOf(2030) == Q::MouthKind::Drink); // Lead Elixir
    // Throwables, crushed, rubbed, injected, the game's own animation: never.
    for (const int g : {1200, 1201, 1210, 1220, 1230, 1240, 1250, 1251, 1260, 1270, 1410, 1500,
                        1501, 1510, 1590, 1300, 1310, 1320, 1330, 705, 1000,
                        100, 1400, 1411, 1412, 200, 2000}) {
        EXPECT_TRUE(Q::MouthOf(g) == Q::MouthKind::None);
    }
    // Every mouth item is used without the animation (UseGoods: Instant or Bullet).
    for (const int g : {1100, 1101, 1110, 1120, 2030, 701, 702, 703, 706}) {
        const Q::UseKind u = Q::Classify(g).use;
        EXPECT_TRUE(u == Q::UseKind::Instant || u == Q::UseKind::Bullet);
    }
    // The goods params' goodsUseAnim (the 1.09 goods dump) agrees for every instant item.
    struct Row {
        int goods, anim;
    };
    for (const Row r : {Row{1100, 0}, Row{1101, 10}, Row{1110, 0}, Row{1120, 10}, Row{2030, 10},
                        Row{701, 28}, Row{702, 28}, Row{703, 28}, Row{705, 28}, Row{706, 28},
                        Row{1300, 1}, Row{1330, 5}, Row{1500, 9}, Row{1510, 9}}) {
        EXPECT_EQ(Q::MouthOf(r.goods), Q::BloodDrinkOf(r.goods) ? Q::MouthKind::Drink : Q::MouthKindOfAnim(r.anim));
    }
}

TEST(QuickMouth, BottledBloodIsDrunkAndNeverStabbedInEitherScheme) {
    for (int scheme : {1,2}) for (int goods : {701,702,703,706}) {
        Rig r(scheme);
        r.in.snapshot.slot[1].goods=goods;
        if(scheme==1) r.Take1(1); else r.Take2(1);
        ASSERT_EQ(r.out.held_goods,goods);
        EXPECT_FALSE(r.out.stab_item);
        r.in.stab=true;r.in.stab_zone="wrist";
        r.Step(180); // holding or touching the wrist must not consume it
        EXPECT_EQ(r.uses,0);
        r.in.stab=false;
        r.MoveHand(Mouth,.4);r.Step(30);
        ASSERT_EQ(r.uses,1);
        EXPECT_EQ(r.sent.goods,goods);
        EXPECT_EQ(r.sent.mouth,static_cast<std::uint8_t>(Q::MouthKind::Drink));
        EXPECT_EQ(r.sent.stab,0);
        r.Result(Q::Outcome::Used);
        EXPECT_TRUE(r.gulp);
        EXPECT_FALSE(r.bite);
    }
}

TEST(QuickMouth, RefusedBloodDrinkStaysHeldWithoutSuccessFeedback) {
    Rig r; r.in.snapshot.slot[1].goods=703;r.Take2(1);
    r.MoveHand(Mouth,.4);r.Step(30);
    ASSERT_EQ(r.uses,1);
    r.Result(Q::Outcome::Denied);
    EXPECT_TRUE(r.deny);EXPECT_FALSE(r.gulp);
    EXPECT_EQ(r.out.held_goods,703);
    r.MoveHand(Rest,.4);r.MoveHand(Mouth,.4);r.Step(30);
    EXPECT_EQ(r.uses,2);
}

TEST(QuickMouth, MouthPointFollowsTheHead) {
    // Level: 8 cm below and 8 cm in front of the eyes.
    EXPECT_NEAR(Mouth.x, 0.0f, 1e-5f);
    EXPECT_NEAR(Mouth.y, 1.52f, 1e-5f);
    EXPECT_NEAR(Mouth.z, -0.08f, 1e-5f);
    // Looking 35 degrees down (the guard pose): the mouth below the eyes and nearer the body.
    const float d = 35.0f * 3.14159265f / 180.0f;
    const Q::V3 down = Q::MouthPoint(Eye, {0.0f, -std::sin(d), -std::cos(d)}, 0.08f, 0.08f);
    EXPECT_NEAR(Q::Length(down - Eye), Q::Length(Mouth - Eye), 1e-4f);
    EXPECT_LT(down.y, Mouth.y);
    EXPECT_GT(down.z, Mouth.z);
    // The head tilted back 30 degrees to drink: the mouth further forward.
    const float b = 30.0f * 3.14159265f / 180.0f;
    const Q::V3 back = Q::MouthPoint(Eye, {0.0f, std::sin(b), -std::cos(b)}, 0.08f, 0.08f);
    EXPECT_LT(back.z, Mouth.z - 0.025f); // 2.9 cm further forward
    // Turned 90 degrees left: the same point turned with it.
    const Q::V3 left = Q::MouthPoint(Eye, {-1.0f, 0.0f, 0.0f}, 0.08f, 0.08f);
    EXPECT_NEAR(left.x, -0.08f, 1e-5f);
    EXPECT_NEAR(left.y, 1.52f, 1e-5f);
    // Straight down: still a point below the eyes (no NaN).
    const Q::V3 floor = Q::MouthPoint(Eye, {0.0f, -1.0f, 0.0f}, 0.08f, 0.08f);
    EXPECT_TRUE(std::isfinite(floor.x) && std::isfinite(floor.y) && std::isfinite(floor.z));
    EXPECT_LT(floor.y, Eye.y);
}

// ---- The gate --------------------------------------------------------------------------------

struct GateRun {
    Q::MouthGate g;
    Q::MouthConfig c{};
    double t = 0.0;
    Q::V3 p = Rest;
    bool fired = false;
    double fired_t = -1.0;
    GateRun() {
        g.Reset(t);
    }
    void Frame(Q::V3 at, Q::V3 v, bool arm = true, bool valid = true) {
        t += Dt;
        p = at;
        if (g.Update(c, t, valid, at, v, Mouth, arm) && !fired) {
            fired = true;
            fired_t = t;
        }
    }
    void Move(Q::V3 to, double seconds, bool arm = true) {
        const Q::V3 from = p;
        const int n = std::max(1, static_cast<int>(std::lround(seconds / Dt)));
        for (int i = 1; i <= n; ++i) {
            const double s = static_cast<double>(i) / n;
            const double q = 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s;
            const double v = (30 * s * s - 60 * s * s * s + 30 * s * s * s * s) / seconds;
            Frame(from + (to - from) * static_cast<float>(q), (to - from) * static_cast<float>(v),
                  arm);
        }
    }
    void Hold(double seconds, bool arm = true) {
        const int n = static_cast<int>(std::lround(seconds / Dt));
        for (int i = 0; i < n; ++i) {
            Frame(p, {}, arm);
        }
    }
};

TEST(QuickMouth, BroughtToTheMouthFires) {
    GateRun r;
    r.Hold(0.3);
    const double start = r.t;
    r.Move(Mouth + Q::V3{0.0f, -0.03f, -0.03f}, 0.4); // to the lips, a natural reach
    r.Hold(0.4);
    ASSERT_TRUE(r.fired);
    // Within the reach's end plus the dwell (and a few frames of slowing down).
    EXPECT_LE(r.fired_t - start, 0.4 + 0.15 + 0.08);
    EXPECT_GE(r.fired_t - start, 0.15);
}

TEST(QuickMouth, AHandPassingTheFaceNeverFires) {
    // Across the face at 2 m/s and on, through the middle of the zone.
    GateRun r;
    r.Hold(0.3);
    r.Move(Mouth + Q::V3{-0.35f, 0.0f, 0.0f}, 0.3);
    r.Move(Mouth + Q::V3{0.35f, 0.0f, 0.0f}, 0.45); // 70 cm in 0.45 s: 2.9 m/s at the mouth
    r.Move(Rest, 0.4);
    EXPECT_FALSE(r.fired);
    // A hand swung up through the chin and the mouth to above the head, quick.
    GateRun w;
    w.Move(Mouth + Q::V3{0.0f, -0.35f, 0.0f}, 0.5);
    w.Move(Mouth + Q::V3{0.0f, 0.3f, 0.0f}, 0.35);
    w.Move(Rest, 0.4);
    EXPECT_FALSE(w.fired);
}

TEST(QuickMouth, OnlyTheZoneCounts) {
    // Held still 16 cm in front of the mouth (outside the 10 cm ball, inside nothing): never.
    GateRun r;
    r.Move(Mouth + Q::V3{0.0f, 0.0f, -0.16f}, 0.4);
    r.Hold(1.0);
    EXPECT_FALSE(r.fired);
    // At the chin's edge, 9 cm below the point: fires.
    GateRun e;
    e.Move(Mouth + Q::V3{0.0f, -0.09f, 0.0f}, 0.4);
    e.Hold(0.5);
    EXPECT_TRUE(e.fired);
}

TEST(QuickMouth, TheGamesArmNeverFires) {
    // The game animating the arm (a dodge, a hit) sweeps the hand to the face: nothing.
    GateRun r;
    r.Move(Mouth, 0.3, false);
    r.Hold(0.6, false);
    EXPECT_FALSE(r.fired);
    // ... the arm back to the player there: the dwell counts from then.
    r.Hold(0.2, true);
    EXPECT_TRUE(r.fired);
}

TEST(QuickMouth, TakenAtTheMouthNeedsToLeaveFirst) {
    // The item came into the hand at the mouth (the stack's next one squeezed there): only after
    // the hand left the mouth and came back.
    GateRun r;
    r.p = Mouth;
    r.Hold(1.0);
    EXPECT_FALSE(r.fired);
    r.Move(Mouth + Q::V3{0.0f, -0.25f, -0.1f}, 0.3);
    r.Move(Mouth, 0.3);
    r.Hold(0.3);
    EXPECT_TRUE(r.fired);
}

TEST(QuickMouth, ConsumedOnceUntilTheHandLeaves) {
    GateRun r;
    r.Move(Mouth, 0.4);
    r.Hold(0.3);
    ASSERT_TRUE(r.fired);
    r.g.Consume();
    r.fired = false;
    r.Hold(1.0);
    EXPECT_FALSE(r.fired);
    r.Move(Rest, 0.3);
    r.Move(Mouth, 0.4);
    r.Hold(0.3);
    EXPECT_TRUE(r.fired);
}

TEST(QuickMouth, TrackingLossNeitherCountsNorBreaks) {
    // Lost under the headset's edge as it reaches the lips: those frames do not count ...
    GateRun l;
    l.Move(Mouth + Q::V3{0.0f, 0.0f, -0.05f}, 0.4);
    for (int i = 0; i < 30; ++i) {
        l.Frame(l.p, {}, true, false);
    }
    EXPECT_FALSE(l.fired);
    // ... and once it is back, the dwell goes on.
    l.Hold(0.2);
    EXPECT_TRUE(l.fired);
}

// ---- left_scheme=2: the trigger takes and holds ---------------------------------------------

TEST(QuickMouth, PelletEatenAtTheMouth) {
    Rig r;
    r.Take2(2);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1110); // in his hand
    EXPECT_EQ(r.uses, 0);              // not at the pull
    EXPECT_TRUE(r.out.block_l2);       // the gun never fires with it in the hand
    r.MoveHand(Mouth + Q::V3{0.0f, -0.02f, -0.02f}, 0.45);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.mouth_uses, 1);
    EXPECT_EQ(r.sent.goods, 1110);
    EXPECT_EQ(static_cast<int>(r.sent.mouth), static_cast<int>(Q::MouthKind::Eat));
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Waiting);
    EXPECT_EQ(r.out.held_goods, 1110); // still in the hand until the game answers
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.bite);
    EXPECT_FALSE(r.gulp);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_TRUE(r.log.find("at the mouth (eat)") != std::string::npos);
    // The trigger still held at the mouth is no shot until it is let go.
    EXPECT_TRUE(r.out.block_l2);
    r.Trigger(0.0f);
    r.Step();
    EXPECT_FALSE(r.out.block_l2);
}

TEST(QuickMouth, ElixirDrunkAtTheMouth) {
    Rig r;
    r.Take2(1);
    EXPECT_EQ(r.out.held_goods, 1120);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(static_cast<int>(r.sent.mouth), static_cast<int>(Q::MouthKind::Drink));
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.gulp);
    EXPECT_FALSE(r.use_feel);
}

TEST(QuickMouth, LetGoAwayFromTheMouthGoesBack) {
    Rig r;
    r.Take2(3); // the Antidote
    EXPECT_EQ(r.out.held_goods, 1100);
    r.MoveHand(Rest + Q::V3{0.05f, 0.15f, -0.05f}, 0.4); // up toward the chest, not the mouth
    r.Step(20);
    r.Trigger(0.0f);
    r.Step();
    EXPECT_EQ(r.uses, 0); // nothing used: the count never changes
    EXPECT_TRUE(r.putback);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Open); // back on the arc
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_TRUE(r.log.find("away from the mouth: back") != std::string::npos);
}

TEST(QuickMouth, TheTriggerPulledDeeperIsNoUse) {
    // The take is the gun's latch (0.45); squeezing on to 1.0 in the hand uses nothing.
    Rig r;
    r.Grip(1.0f);
    r.Step();
    r.Grip(0.0f);
    r.Step();
    r.Hover(2);
    r.Step(20);
    r.Trigger(0.5f);
    r.Step();
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    r.in.hover.slot = -1;
    for (int i = 0; i < 10; ++i) {
        r.Trigger(0.5f + 0.05f * static_cast<float>(i));
        r.Step();
    }
    r.Step(30);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
}

TEST(QuickMouth, GripPutsItBack) {
    Rig r;
    r.Take2(2);
    r.Step(20);
    r.Grip(1.0f);
    r.Step(20);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Open);
    EXPECT_TRUE(r.log.find("grip: put back") != std::string::npos);
}

TEST(QuickMouth, MolotovNearTheFaceStaysAndStillThrows) {
    Rig r;
    r.Take2(0);
    EXPECT_EQ(r.out.held_goods, 1200);
    r.MoveHand(Mouth, 0.4);
    r.Step(90); // a whole second at the mouth
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.throws, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1200);
    // A throw from there: the hand forward at 5 m/s, the trigger let go at speed.
    const Q::V3 v{0.0f, 1.0f, -5.0f};
    for (int i = 0; i < 12; ++i) {
        r.in.hand_v = v * (static_cast<float>(i + 1) / 12.0f);
        r.in.hand_p = r.in.hand_p + r.in.hand_v * static_cast<float>(Dt);
        r.Step();
    }
    r.Trigger(0.0f);
    r.Step(4);
    EXPECT_EQ(r.throws, 1);
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickMouth, UsedAtThePullStaysSoForTheRest) {
    // Fire Paper and Coldblood Dew are used at the pull as before (rubbed, crushed).
    Rig r;
    r.Take2(4);
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.mouth_uses, 0);
    r.Result(Q::Outcome::Used);
    r.Trigger(0.0f);
    r.Step(60);
    Rig d;
    d.Take2(5);
    EXPECT_EQ(d.uses, 1);
    EXPECT_EQ(d.mouth_uses, 0);
}

TEST(QuickMouth, OffUsesAtThePull) {
    Rig r(2, false);
    r.Take2(2);
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.mouth_uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Waiting);
}

TEST(QuickMouth, RefusedAtTheMouthStaysInTheHand) {
    Rig r;
    r.Take2(2);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Denied); // e.g. an item animation still playing
    EXPECT_TRUE(r.deny);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1110);
    // Kept at the mouth: no second try ...
    r.Step(60);
    EXPECT_EQ(r.uses, 1);
    // ... away and back: eaten.
    r.MoveHand(Rest, 0.4);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    EXPECT_EQ(r.uses, 2);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
}

TEST(QuickMouth, RefusedWithTheTriggerLetGoGoesBack) {
    Rig r;
    r.Take2(2);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    r.Trigger(0.0f);
    r.Step();
    r.Result(Q::Outcome::Denied);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Open);
    EXPECT_EQ(r.out.held_goods, -1);
}

TEST(QuickMouth, CannotBeUsedNowIsADeny) {
    Rig r;
    r.Take2(1);
    r.in.snapshot.slot[1].usable = false; // a Blue Elixir already active
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.deny);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
}

TEST(QuickMouth, CooldownWaitsAtTheMouth) {
    // A pellet eaten, the next taken at once and brought straight back: it waits out the 0.5 s.
    Rig r;
    r.Take2(2);
    r.MoveHand(Mouth, 0.3);
    r.Step(20);
    ASSERT_EQ(r.uses, 1);
    const double first = r.t;
    r.Result(Q::Outcome::Used);
    r.Trigger(0.0f);
    r.cfg.use_cooldown_s = 3.0f; // a long cooldown, to see the wait
    r.MoveHand(Rest, 0.2);
    r.Take2(2);
    r.MoveHand(Mouth, 0.3);
    r.Step(20);
    EXPECT_EQ(r.uses, 1);
    while (r.t - first < 3.2) {
        r.Step();
    }
    EXPECT_EQ(r.uses, 2);
}

TEST(QuickMouth, AMenuPutsItAwayUnused) {
    Rig r;
    r.Take2(2);
    r.MoveHand(Mouth + Q::V3{0.0f, 0.0f, -0.2f}, 0.3);
    r.in.gameplay = false; // a menu, the virtual screen, a cinematic
    r.Step();
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
    EXPECT_EQ(r.out.held_goods, -1);
    r.in.gameplay = true;
    r.MoveHand(Mouth, 0.3);
    r.Step(30);
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickMouth, TheGameKeepingTheArmPutsItAway) {
    Rig r;
    r.Take2(2);
    r.in.arm_weight = 0.2f; // a long animation
    r.MoveHand(Mouth, 0.3);
    r.Step(260);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
}

TEST(QuickMouth, NoHeadPoseNothingEaten) {
    Rig r;
    r.in.mouth_valid = false;
    r.Take2(2);
    r.MoveHand(Mouth, 0.3);
    r.Step(40);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
}

// ---- left_scheme=1: the grip takes and holds -----------------------------------------------

TEST(QuickMouth, SchemeOneGripHoldsMouthUses) {
    Rig r(1);
    r.Take1(2);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1110);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    EXPECT_EQ(r.mouth_uses, 1);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.bite);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
}

TEST(QuickMouth, SchemeOneGripReleasedAwayGoesBack) {
    Rig r(1);
    r.Take1(1);
    r.MoveHand(Rest + Q::V3{0.0f, 0.1f, 0.0f}, 0.3);
    r.Grip(0.0f);
    r.Step();
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.putback);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
    EXPECT_TRUE(r.log.find("grip released away from the mouth") != std::string::npos);
}

TEST(QuickMouth, SchemeOneNextOneSqueezedAtTheMouthWaits) {
    // The stack: the next pellet squeezed at the mouth right after eating one is eaten only once
    // the hand left the mouth and came back.
    Rig r(1);
    r.Take1(2);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    ASSERT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Used);
    r.Grip(0.0f);
    r.Step(10);
    r.Grip(1.0f);
    r.Step(); // the next one, at the mouth
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    r.Step(60);
    EXPECT_EQ(r.uses, 1);
    r.MoveHand(Rest, 0.4);
    r.MoveHand(Mouth, 0.4);
    r.Step(25);
    EXPECT_EQ(r.uses, 2);
}

TEST(QuickMouth, SchemeOneMolotovNearTheFaceStays) {
    Rig r(1);
    r.Take1(0);
    r.MoveHand(Mouth, 0.4);
    r.Step(90);
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.throws, 0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
}

} // namespace
