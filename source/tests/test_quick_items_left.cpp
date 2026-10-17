// SPDX-License-Identifier: GPL-2.0-or-later

// The left hand's second layout (left_scheme=2, the user's 2026-10-05 request): the left grip
// opens and closes the quick-item arc, the left trigger takes the card the hand is on (lit
// dwell_s: sweeping across the arc never takes anything), a throwable flies when the trigger is
// let go (the grip's release path, so the same hand motion throws exactly as far), a usable is
// used at the pull, the trigger off the cards is the gun, the grip puts an item back, the arc
// closes after a throw (opt-in: it is back for the next one), and the grip at the left hip draws a
// Blood Vial (core/vr/quick_items.h, core/vr/belt_vial.h). The review of 2026-10-05: a clench
// during a throw is no put-back, a pull is never eaten, an arc opened in a fight closes, the grip
// held through a menu is no squeeze.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/belt_vial.h"
#include "core/vr/quick_items.h"
#include "core/vr/quick_settings.h"
#include "core/vr/two_hand_grip.h"
#include "core/vr/trick_weapons.h"

namespace {

namespace Q = VR::Quick;
namespace B = VR::Belt;

constexpr double Dt = 1.0 / 90.0;

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, 20000, true}; // Molotov
    s.slot[1] = {81, 1120, 3, 18, 20009, true}; // Blue Elixir
    s.slot[2] = {82, 1110, 3, 20, -1, true};    // Beast Blood Pellet
    s.slot[3] = {79, 1411, 1, 31, -1, true};    // Monocular (the game's own use)
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

// Drives a Controller in left_scheme=2 frame by frame. The trigger's latch (the gun's: on at
// 0.45, off at 0.25 in the motion layout) follows its analog value as the presenter's does.
struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{}; // the last request any step sent
    std::string log;   // every step's log since the last ClearLog
    double t = 100.0;

    Rig() {
        cfg.left_scheme = 2;
        cfg.belt_vial = true;
        in.gameplay = true;
        in.body = true;
        in.arm_weight = 1.0f;
        in.hand_valid = true;
        in.hand_p = {-0.2f, 1.1f, -0.3f};
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
            }
            log += out.log.empty() ? "" : out.log + "\n";
            in.x_edge = false;
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
    // A squeeze of the grip and its release (the arc opens or closes on the squeeze).
    void Squeeze() {
        Grip(1.0f);
        Step();
        Grip(0.0f);
        Step();
    }
    void Open() {
        Squeeze();
    }
    // Opens the arc and holds the hand on `slot` past the unfolding and the dwell.
    void OnCard(int slot) {
        Open();
        Hover(slot);
        Step(20);
    }
    // Takes `slot` with the trigger.
    void Take(int slot) {
        OnCard(slot);
        Trigger(1.0f);
        Step();
    }
    void Result(Q::Outcome o) {
        in.have_result = true;
        in.result = {sent.serial, o, "", sent.goods, 5, 4};
        Step();
        in.have_result = false;
    }
};

// ---- The grip opens and closes the arc -------------------------------------------------------

TEST(QuickItemsLeft, GripOpensAndCloses) {
    Rig r;
    r.Grip(1.0f);
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_TRUE(Has(o, Q::Haptic::Unfurl));
    EXPECT_NE(o.log.find("OPEN n=4 sel=0 by grip"), std::string::npos) << o.log;
    EXPECT_EQ(o.scene.state, Q::SceneState::Open);
    EXPECT_TRUE(o.grip_edge);
    // The squeeze that opened it is no grab, nor a close while it is held.
    r.Step(30);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_EQ(r.out.held_goods, -1);
    r.Grip(0.0f);
    r.Step(5);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    // Squeezed again with nothing taken: closed.
    r.Grip(1.0f);
    EXPECT_TRUE(Has(r.Step(), Q::Haptic::Close));
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(r.out.log.find("CLOSE (grip)"), std::string::npos) << r.out.log;
    EXPECT_FALSE(r.out.send);
    // A grip resting half way (the middle finger on it) is no squeeze.
    Rig h;
    h.Grip(0.6f);
    h.Step(30);
    EXPECT_EQ(h.c.GetPhase(), Q::Phase::Idle);
}

TEST(QuickItemsLeft, GripIsNoArcWhenItCannotBeOne) {
    // The trigger held (a clench round the gun), a vial in the hand (the needle's fist), no
    // hunter's hand or no slots: the grip opens nothing.
    for (int k = 0; k < 4; ++k) {
        Rig r;
        if (k == 0) {
            r.Trigger(1.0f);
        } else if (k == 1) {
            r.in.vial_busy = true;
        } else if (k == 2) {
            r.in.body = false;
        } else {
            r.in.snapshot_fresh = false;
        }
        r.Squeeze();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle) << k;
        EXPECT_NE(r.log.find("no arc"), std::string::npos) << k << " " << r.log;
    }
    // Scheme 1: the grip opens nothing (Y does), as before.
    Rig one;
    one.cfg.left_scheme = 1;
    one.Squeeze();
    EXPECT_EQ(one.c.GetPhase(), Q::Phase::Idle);
    // Y still opens and closes it in scheme 2.
    Rig y;
    y.in.y = true;
    y.Step();
    y.in.y = false;
    y.Step();
    EXPECT_EQ(y.c.GetPhase(), Q::Phase::Open);
}

// ---- The trigger takes the card the hand is on -----------------------------------------------

TEST(QuickItemsLeft, TriggerTakesAThrowableAndHoldsIt) {
    Rig r;
    r.OnCard(0);
    EXPECT_TRUE(r.out.block_l2); // the arc open: no gun until a pull is the gun's
    r.Trigger(1.0f);
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(o.held_goods, 1200);
    EXPECT_TRUE(o.block_l2); // never the gun
    EXPECT_TRUE(Has(o, Q::Haptic::Grab));
    EXPECT_NE(o.log.find("GRAB k=0 goods=1200 qty=5 via=hover by the trigger"), std::string::npos)
        << o.log;
    EXPECT_EQ(o.scene.state, Q::SceneState::Holding);
    // Held while the trigger is: fully, relaxed to half, even down to just over the release.
    for (const float v : {1.0f, 0.6f, 0.4f, 0.36f}) {
        r.Trigger(v);
        r.Step(10);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << v;
        EXPECT_TRUE(r.out.block_l2) << v;
    }
    // Let go slowly: back on the arc (the same arc, open), no throw, nothing sent.
    const std::uint32_t serial = r.c.OpenSerial();
    r.Trigger(0.2f);
    const Q::Output& back = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_EQ(r.c.OpenSerial(), serial);
    EXPECT_FALSE(back.send);
    EXPECT_EQ(back.held_goods, -1);
    EXPECT_TRUE(Has(back, Q::Haptic::PutBack));
    EXPECT_NE(back.log.find("trigger let go slowly"), std::string::npos) << back.log;
}

TEST(QuickItemsLeft, SweepingAcrossTheArcNeverTakes) {
    // The trigger down while the hand sweeps over the cards (each lit ~50 ms, 9 cm apart): nothing
    // taken or used. The pull began on a card lit under the dwell; the hand slipping off it makes
    // it the gun's (the arc closes): a pull is never eaten (review 2026-10-05).
    Rig r;
    r.Open();
    r.Step(10); // unfolded
    r.Hover(2);
    r.Step(2);
    r.Trigger(1.0f);
    r.Step(2);
    EXPECT_TRUE(r.out.block_l2); // waiting for the dwell
    r.Hover(1); // the hand crosses into the next card ...
    r.Step();
    EXPECT_TRUE(r.out.block_l2); // (within late_move_m of the edge: still the pull's jerk)
    r.in.hand_p.x += 0.09f; // ... and sweeps on
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(o.block_l2); // the gun, this frame
    EXPECT_EQ(r.sent.serial, 0u);
    EXPECT_NE(r.log.find("(under the dwell): the gun"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("CLOSE (trigger (gun))"), std::string::npos) << r.log;
    // Let go and pulled again on a card lit long since: taken (a usable: used at the pull).
    Rig u;
    u.OnCard(1);
    u.Trigger(1.0f);
    u.Step();
    EXPECT_EQ(u.c.GetPhase(), Q::Phase::Waiting); // the Blue Elixir, used at the pull
    EXPECT_EQ(u.sent.action, Q::Action::Use);
    EXPECT_EQ(u.sent.goods, 1120);
}

TEST(QuickItemsLeft, PullBeforeTheDwellWaitsForIt) {
    Rig r;
    r.Open();
    r.Step(10);
    r.Hover(0);
    r.Step(3); // lit 33 ms
    r.Trigger(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_TRUE(r.out.block_l2);
    int frames = 1;
    while (r.c.GetPhase() == Q::Phase::Open && frames < 30) {
        r.Step();
        ++frames;
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    // Taken once lit 100 ms (9 frames at 90 Hz from the hover), not later.
    EXPECT_LE(3 + frames, 10) << frames;
    EXPECT_NE(r.log.find("waited for the dwell"), std::string::npos) << r.log;
    // Let go before the dwell: nothing.
    Rig q;
    q.Open();
    q.Step(10);
    q.Hover(0);
    q.Step(2);
    q.Trigger(1.0f);
    q.Step(2);
    q.Trigger(0.0f);
    q.Step(20);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Open);
    EXPECT_NE(q.log.find("nothing taken"), std::string::npos) << q.log;
    EXPECT_EQ(q.sent.serial, 0u);
    // The pull's own jerk takes the hand just off the card (2 cm): still that card, at the dwell.
    Rig j;
    j.Open();
    j.Step(10);
    j.Hover(0);
    j.Step(3);
    j.Trigger(1.0f);
    j.Step();
    j.in.hand_p.y -= 0.02f;
    j.Hover(-1);
    j.Step(8);
    EXPECT_EQ(j.c.GetPhase(), Q::Phase::Holding);
    EXPECT_NE(j.log.find("just off it"), std::string::npos) << j.log;
}

TEST(QuickItemsLeft, TriggerOffTheCardsIsTheGun) {
    // The user's rule: the arc open, nothing held, the gun in the hand: off the cards the pull
    // fires it at once and the arc closes, even with the grip that opened it still squeezed (no
    // fist rule here: the grip is no grab).
    for (const bool grip_held : {false, true}) {
        Rig r;
        r.Grip(1.0f);
        r.Step();
        if (!grip_held) {
            r.Grip(0.0f);
        }
        r.Step(20);
        EXPECT_TRUE(r.out.block_l2);
        r.Trigger(1.0f);
        EXPECT_FALSE(r.Step().block_l2) << grip_held;
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle) << grip_held;
        EXPECT_NE(r.out.log.find("CLOSE (trigger (gun))"), std::string::npos) << r.out.log;
        EXPECT_FALSE(r.Step().block_l2); // held: the gun's L2 stays down
    }
    // Right after the open (the arc still unfolding, no card lit): the gun.
    Rig q;
    q.Grip(1.0f);
    q.Step();
    q.Trigger(1.0f);
    EXPECT_FALSE(q.Step().block_l2);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
    // Closed (Idle): the trigger is the gun's as without the arc.
    Rig idle;
    idle.Trigger(1.0f);
    EXPECT_FALSE(idle.Step().block_l2);
}

TEST(QuickItemsLeft, LateHoverOfALitCardTakesIt) {
    // The pull's own jerk moved the hand just off a card it had lit: that card.
    Rig r;
    r.OnCard(0);
    r.Hover(-1);
    r.Step(3); // 33 ms off it
    r.Trigger(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_NE(r.log.find("via=late hover"), std::string::npos) << r.log;
    // A card passed over (lit < dwell) and left: the gun.
    Rig q;
    q.Open();
    q.Step(10);
    q.Hover(0);
    q.Step(3);
    q.Hover(-1);
    q.Step(2);
    q.Trigger(1.0f);
    EXPECT_FALSE(q.Step().block_l2);
    EXPECT_EQ(q.c.GetPhase(), Q::Phase::Idle);
    // Long after leaving it (> quick_arc_grace_ms): the gun.
    Rig g;
    g.OnCard(0);
    g.Hover(-1);
    g.Step(20);
    g.Trigger(1.0f);
    EXPECT_FALSE(g.Step().block_l2);
    EXPECT_EQ(g.c.GetPhase(), Q::Phase::Idle);
}

// ---- Usables: used at the pull ---------------------------------------------------------------

TEST(QuickItemsLeft, UsableUsedAtThePull) {
    Rig r;
    r.Take(1); // Blue Elixir
    EXPECT_TRUE(r.out.send);
    EXPECT_EQ(r.out.request.action, Q::Action::Use);
    EXPECT_EQ(r.out.request.goods, 1120);
    EXPECT_EQ(r.out.request.select, 1); // selected in the game too
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Waiting);
    EXPECT_TRUE(Has(r.out, Q::Haptic::Pick));
    EXPECT_FALSE(Has(r.out, Q::Haptic::Grab));
    EXPECT_TRUE(r.out.block_l2);
    r.Result(Q::Outcome::Used);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle); // done: the arc closes
    EXPECT_TRUE(Has(r.out, Q::Haptic::Use));
    // The trigger that used it is no shot until it is let go.
    r.Step(5);
    EXPECT_TRUE(r.out.block_l2);
    r.Trigger(0.0f);
    r.Step();
    r.Trigger(1.0f);
    EXPECT_FALSE(r.Step().block_l2);
}

TEST(QuickItemsLeft, UsableTheGameAnimates) {
    // The Monocular: selected, then the game's own Square once the HUD shows it.
    Rig r;
    r.Take(3);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::SquareWait);
    EXPECT_EQ(r.sent.select, 3);
    r.in.snapshot.selected = 3;
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.square_press);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_TRUE(o.block_l2); // the trigger still down: no shot
}

TEST(QuickItemsLeft, RefusedUseKeepsTheArc) {
    Rig r;
    r.in.snapshot.slot[1].usable = false;
    r.Take(1);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_FALSE(r.out.send);
    EXPECT_TRUE(Has(r.out, Q::Haptic::Deny));
    EXPECT_TRUE(r.out.block_l2);
    // The game refuses a use it was sent: back on the arc.
    Rig g;
    g.Take(2);
    g.Result(Q::Outcome::Denied);
    EXPECT_EQ(g.c.GetPhase(), Q::Phase::Open);
    EXPECT_NE(g.out.log.find("refused: back on the arc"), std::string::npos) << g.out.log;
    // A second use within the cooldown: refused.
    Rig c;
    c.Take(2);
    c.Result(Q::Outcome::Used);
    c.Trigger(0.0f);
    c.Step();
    c.OnCard(2);
    c.Trigger(1.0f);
    c.Step();
    EXPECT_EQ(c.c.GetPhase(), Q::Phase::Open);
    EXPECT_NE(c.log.find("cooldown"), std::string::npos) << c.log;
}

// ---- Throws: the trigger let go --------------------------------------------------------------

TEST(QuickItemsLeft, TriggerReleaseThrows) {
    Rig r;
    r.Take(0);
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    // The finger opens: 0.30 is past the item's release (0.35) but the gun's latch is still on.
    r.Trigger(0.30f);
    ASSERT_TRUE(r.in.trigger_latch);
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.send);
    EXPECT_EQ(o.request.action, Q::Action::Throw);
    EXPECT_EQ(o.request.goods, 1200);
    EXPECT_EQ(o.held_goods, -1);
    const Q::Config d{};
    const float speed = std::hypot(o.request.v_world[0], o.request.v_world[1], o.request.v_world[2]);
    EXPECT_NEAR(speed, d.throw_gain * Q::LaunchCurve(std::hypot(1.0f, 5.0f), d.throw_gain_curve),
                0.05f * d.throw_gain);
    EXPECT_TRUE(o.block_l2); // the trigger still past the latch: no shot with the throw
    r.Trigger(0.0f);
    EXPECT_FALSE(r.Step().block_l2); // let go: the gun is free while the throw is answered
    r.Result(Q::Outcome::Thrown);
    // The arc closes: the next pull is the gun's (quick_arc_reopen_s=0, the default).
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(r.out.scene.state, Q::SceneState::Closed);
    r.Trigger(1.0f);
    EXPECT_FALSE(r.Step().block_l2);
    // Opt-in (quick_arc_reopen_s > 0): the arc is back where it was for the next one.
    Rig b;
    b.cfg.reopen_s = 2.0f;
    b.Take(0);
    b.in.hand_v = {0.0f, 1.0f, -5.0f};
    b.Step(6);
    b.Trigger(0.0f);
    b.Step();
    b.Result(Q::Outcome::Thrown);
    EXPECT_EQ(b.c.GetPhase(), Q::Phase::Open);
    EXPECT_EQ(b.out.scene.state, Q::SceneState::Open);
    EXPECT_NE(b.out.log.find("the arc is back for the next one"), std::string::npos) << b.out.log;
}

TEST(QuickItemsLeft, ArcIsBackAfterAThrow) {
    // Opt-in: quick_arc_reopen_s (0 by default since the review of 2026-10-05).
    const auto throw_one = [](Rig& r) {
        if (r.cfg.reopen_s == 0.0f && r.cfg.stack) {
            r.cfg.reopen_s = 2.0f;
        }
        r.Take(0);
        r.in.hand_v = {0.0f, 1.0f, -5.0f};
        r.Step(6);
        r.Trigger(0.0f);
        r.Step();
        r.in.hand_v = {};
        r.Result(Q::Outcome::Thrown);
    };
    // Nothing reached for: closed again after quick_arc_reopen_s.
    Rig r;
    throw_one(r);
    const std::uint32_t serial = r.c.OpenSerial();
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.Hover(-1);
    r.Step(static_cast<int>(r.cfg.reopen_s / Dt) - 3);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.Step(5);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(r.log.find("CLOSE (the next one was not taken)"), std::string::npos) << r.log;
    // The next one taken from it: the same placement (no new open), a card lit reopen_dwell_s
    // (0.25 s: the hand coming back to rest on a card is no reach), a pull.
    Rig n;
    throw_one(n);
    const std::uint32_t before = n.c.OpenSerial();
    n.Hover(0);
    n.Step(3);
    EXPECT_EQ(n.c.GetPhase(), Q::Phase::Open); // not before the arc fades in (hover_delay_s)
    n.Step(15); // lit ~0.11 s: a pull now waits for the longer dwell
    n.Trigger(1.0f);
    n.Step();
    EXPECT_EQ(n.c.GetPhase(), Q::Phase::Open);
    n.Step(15);
    EXPECT_EQ(n.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(n.c.OpenSerial(), before);
    // Put back: an arc as one opened (no reopen timer; idle_close2_s with no card lit).
    n.Trigger(0.2f);
    n.Step();
    EXPECT_EQ(n.c.GetPhase(), Q::Phase::Open);
    n.Hover(-1);
    n.Step(static_cast<int>(n.cfg.reopen_s / Dt) + 20);
    EXPECT_EQ(n.c.GetPhase(), Q::Phase::Open);
    // The hand back to rest on a card after the throw (lit, nothing pulled): it still closes at
    // reopen_s (the 2026-10-05 mock: a card lit 66-143 ms kept it open until the 6 s idle close).
    Rig rest;
    throw_one(rest);
    rest.Step(12);
    rest.Hover(1);
    rest.Step(static_cast<int>(rest.cfg.reopen_s / Dt) - 12 + 3);
    EXPECT_EQ(rest.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(rest.log.find("CLOSE (the next one was not taken)"), std::string::npos) << rest.log;
    // quick_arc_reopen_s=0 or quick_arc_stack=0: closed after the throw.
    for (const int k : {0, 1}) {
        Rig o;
        if (k == 0) {
            o.cfg.reopen_s = -1.0f; // (throw_one turns 0 into 2: below 0 counts as off too)
        } else {
            o.cfg.stack = false;
        }
        throw_one(o);
        EXPECT_EQ(o.c.GetPhase(), Q::Phase::Idle) << k;
    }
    // Off the cards the trigger after a throw is the gun (and closes it).
    Rig g;
    throw_one(g);
    g.Step(5);
    g.Trigger(1.0f);
    EXPECT_FALSE(g.Step().block_l2);
    EXPECT_EQ(g.c.GetPhase(), Q::Phase::Idle);
    (void)serial;
}

TEST(QuickItemsLeft, GripPutsTheItemBack) {
    Rig r;
    r.Take(0);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    const std::uint32_t serial = r.c.OpenSerial();
    r.Grip(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding); // putback_wait_s (0.12 s), the hand still
    r.Step(10);
    const Q::Output& o = r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open); // back on the arc, which stays open
    EXPECT_EQ(r.c.OpenSerial(), serial);
    EXPECT_EQ(o.held_goods, -1);
    EXPECT_TRUE(Has(o, Q::Haptic::PutBack));
    EXPECT_FALSE(o.send);
    // The trigger still down: neither the gun nor another take; let go: nothing.
    r.Step(20);
    EXPECT_TRUE(r.out.block_l2);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.Trigger(0.0f);
    r.Step(3);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_FALSE(r.out.send);
    // The grip let go and squeezed again (nothing taken): closed.
    r.Grip(0.0f);
    r.Step();
    r.Grip(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    // Y and X put it away and close, as in scheme 1.
    Rig y;
    y.Take(0);
    y.in.y = true;
    y.Step();
    EXPECT_EQ(y.c.GetPhase(), Q::Phase::Idle);
    Rig x;
    x.Take(0);
    x.in.x_edge = true;
    x.Step();
    EXPECT_EQ(x.c.GetPhase(), Q::Phase::Idle);
}

// ---- The trigger's release throws as the grip's: the same distances ---------------------------

// The user's Quest over Link: 124 input frames a second, every pose predicted 45 ms ahead.
constexpr double LinkDt = 1.0 / 124.0;
constexpr float LinkLead = 0.045f;

// One throw at the Link's input rate, the hand at the true speed `s(t)` along `dir`; the release
// (the grip in scheme 1, the trigger in scheme 2) follows `finger(t)` (1 held .. 0 open, t from
// the throw's own zero). The runtime "holds" the velocity in its prediction (the user's): the
// display-time pose reads s(f), the fresh pose s(f) too.
Q::V3 ReplayThrow(int scheme, const std::function<float(double)>& s,
                  const std::function<float(double)>& finger, Q::V3 dir) {
    Rig r;
    r.cfg.left_scheme = scheme;
    if (scheme == 2) {
        r.Take(0);
    } else {
        // Scheme 1: Y, the hand on the card, the grip.
        r.in.y = true;
        r.Step();
        r.in.y = false;
        r.Step();
        r.Hover(0);
        r.Step(9);
        r.Grip(1.0f);
        r.Step();
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding) << scheme;
    const double t0 = r.t + 0.6;
    for (double t = r.t + LinkDt; t < t0 + 0.4; t += LinkDt) {
        const double f = t - t0;
        r.t = t;
        r.in.t = t;
        r.in.dt = static_cast<float>(LinkDt);
        r.in.hover.steady_ns = static_cast<std::int64_t>(t * 1e9);
        r.in.pose_lead_s = LinkLead;
        r.in.hand_v = dir * s(f);
        r.in.hand_now_valid = true;
        r.in.hand_v_now = dir * s(f);
        if (scheme == 2) {
            r.Trigger(finger(f));
        } else {
            r.Grip(finger(f));
        }
        r.c.Update(r.cfg, r.in, r.out);
        if (r.out.send && r.out.request.action == Q::Action::Throw) {
            return {r.out.request.v_world[0], r.out.request.v_world[1], r.out.request.v_world[2]};
        }
    }
    return {};
}

float LandsAt(Q::V3 launch, float h) {
    const Q::QuickSettings s{};
    const float cap = Q::ThrowCapAt(1.6f * 13.42f, 1.5f, 9.81f, Q::PitchOf(launch),
                                    s.throw_cap_flat_deg, 14.0f);
    const Q::V3 v = Q::SoftCap(launch, cap, s.throw_cap_knee);
    const float speed = Q::Length(v);
    const float deg = Q::PitchOf(v) * 0.01745329f;
    const float vx = speed * std::cos(deg);
    const float vy = speed * std::sin(deg);
    return vx * (vy + std::sqrt(vy * vy + 2.0f * 9.81f * h)) / 9.81f;
}

// The user's 9 real Molotov throws (2026-10-05 00:56 headset log; tests/test_quick_items.cpp
// UserThrowsReplay): the hand's speed as read then, the runtime's fastest pose just past the
// release, the launch's pitch and the release's height. Released by the grip (scheme 1) and by the
// trigger (scheme 2) with the same hand motion: the same launch, the same landing.
TEST(QuickItemsLeft, TriggerThrowsAsFarAsTheGrip) {
    struct UserThrow {
        float hand, after, pitch, height;
    };
    constexpr std::array<UserThrow, 9> user{{
        {2.08f, 2.23f, -6.7f, 1.69f},
        {2.66f, 4.11f, 19.9f, 1.70f},
        {2.31f, 3.26f, 47.2f, 1.39f},
        {1.80f, 2.33f, 1.1f, 1.46f},
        {4.52f, 6.16f, 17.0f, 1.62f},
        {3.66f, 3.82f, 35.5f, 1.26f},
        {2.54f, 4.75f, 0.6f, 1.63f},
        {2.00f, 3.16f, -12.3f, 1.52f},
        {2.61f, 5.27f, -1.9f, 1.43f},
    }};
    std::string table;
    float worst_ramp = 0.0f;
    for (const UserThrow& u : user) {
        const float hand = u.hand, after = u.after;
        const auto speed = [hand, after](double t) {
            if (t < -0.30) {
                return 0.0f;
            }
            if (t < -0.045) {
                return hand * static_cast<float>((t + 0.30) / 0.255);
            }
            if (t < 0.0) {
                return hand + (after - hand) * static_cast<float>((t + 0.045) / 0.045);
            }
            return std::max(0.0f, after * static_cast<float>(1.0 - t / 0.10));
        };
        const float p = u.pitch * 0.01745329f;
        const Q::V3 dir{0.0f, std::sin(p), -std::cos(p)};
        // Opened at once at the throw's zero: the very same frame either way.
        const auto snap = [](double t) { return t < 0.0 ? 1.0f : 0.0f; };
        const Q::V3 grip = ReplayThrow(1, speed, snap, dir);
        const Q::V3 trigger = ReplayThrow(2, speed, snap, dir);
        ASSERT_GT(Q::Length(grip), 1.0f);
        EXPECT_NEAR(trigger.x, grip.x, 1e-4f);
        EXPECT_NEAR(trigger.y, grip.y, 1e-4f);
        EXPECT_NEAR(trigger.z, grip.z, 1e-4f);
        // A real finger opens over ~40 ms (1 -> 0 from the throw's zero): the grip leaves at its
        // 0.45, the trigger at 0.35 (4 ms later), within a Link frame of each other.
        const auto ramp = [](double t) {
            return static_cast<float>(std::clamp(1.0 - t / 0.040, 0.0, 1.0));
        };
        const Q::V3 grip_ramp = ReplayThrow(1, speed, ramp, dir);
        const Q::V3 trigger_ramp = ReplayThrow(2, speed, ramp, dir);
        const float a = LandsAt(grip_ramp, u.height);
        const float b = LandsAt(trigger_ramp, u.height);
        worst_ramp = std::max(worst_ramp, std::abs(b - a) / a);
        EXPECT_NEAR(b, a, 0.05f * a);
        char line[128];
        std::snprintf(line, sizeof(line), " %.2f m/s %+.0f deg: grip %.1f m, trigger %.1f m;",
                      u.after, u.pitch, a, b);
        table += line;
    }
    std::printf("[ QuickItemsLeft.TriggerThrowsAsFarAsTheGrip ] the same hand, released over 40 ms "
                "(grip / trigger):%s worst %.1f%%\n",
                table.c_str(), 100.0f * worst_ramp);
}

TEST(QuickItemsLeft, TriggerReleaseWaitsForThePeakAsTheGrip) {
    // Let go while the hand still speeds up (16 ms before its 4 m/s peak): the item stays in the
    // hand until the peak (throw_rise_wait_ms), exactly as the grip's opening.
    const auto swing = [](double t) {
        constexpr float V = 4.0f;
        if (t < -0.15 || t >= 0.10) {
            return 0.0f;
        }
        if (t < 0.0) {
            return V * static_cast<float>(std::pow(std::sin(0.5 * 3.14159265 * (t + 0.15) / 0.15),
                                                   2.0));
        }
        return V * static_cast<float>(std::pow(std::cos(0.5 * 3.14159265 * t / 0.10), 2.0));
    };
    const auto snap = [](double t) { return t < -0.016 ? 1.0f : 0.0f; };
    const Q::V3 fwd = Q::Normalize({0.0f, 0.1f, -1.0f});
    const Q::V3 grip = ReplayThrow(1, swing, snap, fwd);
    const Q::V3 trigger = ReplayThrow(2, swing, snap, fwd);
    EXPECT_GT(Q::Length(trigger), 0.0f);
    EXPECT_NEAR(Q::Length(trigger), Q::Length(grip), 1e-3f);
}

TEST(QuickItemsLeft, TrackingJumpNeverThrowsFromTheTrigger) {
    // A one-frame tracking jump while the item is held, then a slow release: put back (the spike
    // filter and throw_max_hand_speed are the grip's, shared).
    Rig r;
    r.Take(0);
    r.in.hand_v = {0.0f, 0.0f, -0.3f};
    r.Step(10);
    r.in.hand_v = {0.0f, -40.0f, 0.0f};
    r.in.hand_v_now = {0.0f, -40.0f, 0.0f};
    r.in.hand_now_valid = true;
    r.Step();
    r.in.hand_v = r.in.hand_v_now = {0.0f, 0.0f, -0.3f};
    r.Step(10);
    r.Trigger(0.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    EXPECT_FALSE(r.sent.action == Q::Action::Throw);
}

// ---- The belt vial ---------------------------------------------------------------------------

TEST(QuickItemsLeft, GripAtTheHipDrawsAVial) {
    Rig r;
    r.in.at_hip = true;
    r.Grip(1.0f);
    const Q::Output& o = r.Step();
    EXPECT_TRUE(o.draw_vial);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle); // never the arc
    EXPECT_NE(o.log.find("BELT VIAL"), std::string::npos) << o.log;
    // Held, then again with the vial in the hand: nothing.
    r.Step(5);
    EXPECT_FALSE(r.out.draw_vial);
    r.Grip(0.0f);
    r.Step();
    r.in.vial_busy = true;
    r.Grip(1.0f);
    r.Step();
    EXPECT_FALSE(r.out.draw_vial);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(r.out.log.find("a vial is already in the hand"), std::string::npos) << r.out.log;
    // With the arc open: the vial (the newest intent), the arc closes.
    Rig a;
    a.Open();
    a.Step(10);
    a.in.at_hip = true;
    a.Grip(1.0f);
    a.Step();
    EXPECT_TRUE(a.out.draw_vial);
    EXPECT_EQ(a.c.GetPhase(), Q::Phase::Idle);
    // With an item in the hand: the grip puts it back (not a vial).
    Rig h;
    h.Take(0);
    h.in.at_hip = true;
    h.Grip(1.0f);
    h.Step(12);
    EXPECT_FALSE(h.out.draw_vial);
    EXPECT_EQ(h.c.GetPhase(), Q::Phase::Open);
    // The trigger held (a clench): nothing.
    Rig t;
    t.in.at_hip = true;
    t.Trigger(1.0f);
    t.Step();
    t.Grip(1.0f);
    t.Step();
    EXPECT_FALSE(t.out.draw_vial);
    EXPECT_EQ(t.c.GetPhase(), Q::Phase::Idle);
    // belt_vial off (or scheme 1): the hip is like anywhere else (the arc opens / nothing).
    Rig off;
    off.cfg.belt_vial = false;
    off.in.at_hip = true;
    off.Grip(1.0f);
    off.Step();
    EXPECT_FALSE(off.out.draw_vial);
    EXPECT_EQ(off.c.GetPhase(), Q::Phase::Open);
    Rig one;
    one.cfg.left_scheme = 1;
    one.in.at_hip = true;
    one.Grip(1.0f);
    one.Step();
    EXPECT_FALSE(one.out.draw_vial);
    // A throw in flight: the hand is free, the vial is drawn, the arc stays closed after it.
    Rig f;
    f.Take(0);
    f.in.hand_v = {0.0f, 1.0f, -5.0f};
    f.Step(6);
    f.Trigger(0.0f);
    f.Step();
    ASSERT_EQ(f.c.GetPhase(), Q::Phase::Waiting);
    f.in.at_hip = true;
    f.Grip(1.0f);
    f.in.have_result = true;
    f.in.result = {f.sent.serial, Q::Outcome::Thrown, "", 1200, 5, 4};
    f.Step();
    EXPECT_TRUE(f.out.draw_vial);
    EXPECT_EQ(f.c.GetPhase(), Q::Phase::Idle);
}

// The hip's box (core/vr/belt_vial.h), standing and seated.
TEST(QuickItemsLeft, HipZone) {
    const B::P3 head{0.0f, 1.65f, 0.0f};
    const B::P3 fwd{0.0f, 0.0f, -1.0f}; // facing -Z: the left is -X
    const auto at = [&](float ahead_of_eyes, float left, float below,
                        const B::Zone& z = B::Standing) {
        return B::Locate(head, {-left, head.y - below, -ahead_of_eyes}, fwd, z);
    };
    // Beside the left hip, a hand hanging at the left side, the left front of the thigh.
    EXPECT_TRUE(at(-0.10f, 0.24f, 0.62f).inside);
    EXPECT_TRUE(at(-0.08f, 0.22f, 0.85f).inside);
    EXPECT_TRUE(at(0.05f, 0.23f, 0.70f).inside);
    // 16 / 20 cm out: the torso's stab zone (18 cm round a line 5 cm before the eyes) or its edge.
    EXPECT_FALSE(at(-0.10f, 0.16f, 0.62f).inside);
    EXPECT_FALSE(at(0.05f, 0.20f, 0.70f).inside);
    // In front of the belly (the stab zone, the arc's place), the chest, the right hip, above the
    // belt, the gun held at the hip pointing ahead, a hand reaching far out.
    EXPECT_FALSE(at(0.25f, 0.0f, 0.65f).inside);
    EXPECT_FALSE(at(0.30f, 0.10f, 0.35f).inside);
    EXPECT_FALSE(at(-0.10f, -0.24f, 0.62f).inside);
    EXPECT_FALSE(at(-0.10f, 0.24f, 0.35f).inside);
    EXPECT_FALSE(at(0.35f, 0.18f, 0.60f).inside);
    EXPECT_FALSE(at(-0.10f, 0.60f, 0.62f).inside);
    // Its numbers: from the body's axis 10 cm behind the eyes.
    const B::Place p = at(-0.10f, 0.24f, 0.62f);
    EXPECT_NEAR(p.ahead, 0.0f, 1e-5f);
    EXPECT_NEAR(p.out, 0.24f, 1e-5f);
    EXPECT_NEAR(p.below, 0.62f, 1e-5f);
    // Seated: beside the lap yes, on it no.
    EXPECT_TRUE(at(0.10f, 0.28f, 0.60f, B::Seated).inside);
    EXPECT_FALSE(at(0.25f, 0.05f, 0.60f, B::Seated).inside);
    // belt_vial_zone: a bigger box reaches further out, a smaller one less.
    EXPECT_FALSE(at(-0.10f, 0.50f, 0.62f).inside);
    EXPECT_TRUE(at(-0.10f, 0.50f, 0.62f, B::Scaled(B::Standing, 1.5f)).inside);
    EXPECT_FALSE(at(-0.08f, 0.22f, 0.98f, B::Scaled(B::Standing, 0.7f)).inside);
}

// The torso's heading: a glance down at the hip leaves the box on the hip; a real turn turns it.
TEST(QuickItemsLeft, HipZoneFollowsTheBodyNotAGlance) {
    constexpr float Rad = 3.14159265f / 180.0f;
    const auto view = [](float yaw_deg, float pitch_deg) {
        // yaw + = to the left (toward -X from -Z), pitch + = up.
        const float y = yaw_deg * Rad, p = pitch_deg * Rad;
        return B::P3{-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
    };
    B::Torso torso;
    for (int i = 0; i < 200; ++i) {
        torso.Update(view(0.0f, -5.0f), 1.0f / 90.0f);
    }
    const B::P3 head{0.0f, 1.65f, 0.0f};
    const B::P3 hip{-0.24f, 1.65f - 0.65f, 0.10f}; // beside the left hip (the axis 10 cm back)
    EXPECT_TRUE(B::Locate(head, hip, torso.Forward(), B::Standing).inside);
    // Looking down at it: 60 degrees left, 50 down, for 0.6 s (the head's eyes move a little).
    for (int i = 0; i < 54; ++i) {
        torso.Update(view(60.0f, -50.0f), 1.0f / 90.0f);
    }
    const B::P3 head_down{-0.04f, 1.60f, -0.03f};
    EXPECT_TRUE(B::Locate(head_down, hip, torso.Forward(), B::Standing).inside);
    // Measured in the head's own heading instead, the hip would fall outside the box.
    EXPECT_FALSE(B::Locate(head_down, hip, view(60.0f, 0.0f), B::Standing).inside);
    // A real quarter turn to the left, looking ahead: the box turns with the body within ~2 s.
    for (int i = 0; i < 200; ++i) {
        torso.Update(view(90.0f, -5.0f), 1.0f / 90.0f);
    }
    const B::P3 f = torso.Forward();
    EXPECT_NEAR(f.x, -1.0f, 0.02f);
    EXPECT_NEAR(f.z, 0.0f, 0.15f);
    // Straight down (no heading): kept.
    const B::P3 before = torso.Forward();
    torso.Update({0.0f, -1.0f, 0.0f}, 0.5f);
    EXPECT_NEAR(torso.Forward().x, before.x, 1e-6f);
    EXPECT_NEAR(torso.Forward().z, before.z, 1e-6f);
}

// ---- The review of 2026-10-05 -----------------------------------------------------------------

// A throw's wind-up, the fingers tightening round the controller half way (the grip to 0.85): no
// put-back, the throw flies (the reviewer's repro: "grip: put back", nothing thrown).
TEST(QuickItemsLeft, ClenchDuringAThrowIsNoPutBack) {
    for (const int clench_at : {0, 1, 2, 6, 9}) { // 0: the hand still as the wind-up starts
        Rig r;
        r.Take(0);
        r.Step(5);
        for (int i = 0; i < 12; ++i) {
            const float sp = 0.5f * static_cast<float>(i);
            r.in.hand_v = {0.0f, 0.3f * sp, -sp};
            if (i == clench_at) {
                r.Grip(0.85f);
            }
            r.Step();
        }
        r.Trigger(0.0f); // let go at 5.5 m/s
        r.Step(3);
        EXPECT_EQ(r.sent.action, Q::Action::Throw) << clench_at << " " << r.log;
        EXPECT_EQ(r.log.find("grip: put back"), std::string::npos) << clench_at << " " << r.log;
    }
    // The same squeeze with the hand still: a put-back (after putback_wait_s).
    Rig s;
    s.Take(0);
    s.in.hand_v = {0.0f, 0.0f, -0.2f};
    s.Step(5);
    s.Grip(0.85f);
    s.Step(12);
    EXPECT_EQ(s.c.GetPhase(), Q::Phase::Open);
    EXPECT_NE(s.log.find("grip: put back"), std::string::npos) << s.log;
    // A squeeze with the hand still, the throw starting right after it: the throw.
    Rig t;
    t.Take(0);
    t.Step(5);
    t.Grip(0.85f);
    t.Step(2);
    t.in.hand_v = {0.0f, 0.5f, -4.0f};
    t.Step(4);
    t.Trigger(0.0f);
    t.Step(3);
    EXPECT_EQ(t.sent.action, Q::Action::Throw) << t.log;
    EXPECT_NE(t.log.find("grip put-back dropped"), std::string::npos) << t.log;
}

// A late-hover pull takes the card only with the hand still where it left it (the pull's jerk):
// the reviewer's repro, the hand passing a Blue Elixir on its way to aim, used it and fired no
// shot.
TEST(QuickItemsLeft, LateHoverNeedsTheHandWhereItLeftTheCard) {
    Rig r;
    r.Squeeze();
    r.Hover(-1);
    r.Step(10);
    r.Hover(1); // the Blue Elixir lit 133 ms on the way up
    r.Step(12);
    r.Hover(-1); // the hand leaves it ...
    r.Step();
    r.in.hand_p.y += 0.07f; // ... and goes on up to aim (7 cm in 100 ms)
    r.Step(8);
    r.Trigger(1.0f);
    const Q::Output& o = r.Step();
    EXPECT_NE(r.sent.action, Q::Action::Use);
    EXPECT_EQ(r.sent.serial, 0u);
    EXPECT_FALSE(o.block_l2); // the gun
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    // The mock (2026-10-05, c2_items J4): the hand 10 cm up off the Antidote in 80 ms and still;
    // the hover's loss seen only once it was there (the game's frame, the 8 cm hold); the trigger
    // 0.12 s later used the Antidote. The hand's travel around the loss counts, not where it was
    // when the loss was seen.
    Rig m;
    m.Squeeze();
    m.Hover(-1);
    m.Step(10);
    m.Hover(3);
    m.Step(25);
    for (int i = 1; i <= 7; ++i) { // 10 cm in 78 ms, the card still lit (the lag)
        m.in.hand_p.y += 0.10f / 7.0f;
        m.Step();
    }
    m.Hover(-1);
    m.Step(11);
    m.Trigger(1.0f);
    EXPECT_FALSE(m.Step().block_l2) << m.log;
    EXPECT_EQ(m.sent.serial, 0u) << m.log;
    // A hand resting at the card's edge that drifts 1.5 cm off it, then the pull: still the card.
    Rig d;
    d.Squeeze();
    d.Hover(-1);
    d.Step(10);
    d.Hover(0);
    d.Step(25);
    d.in.hand_p.x -= 0.015f;
    d.Step();
    d.Hover(-1);
    d.Step(5);
    d.Trigger(1.0f);
    d.Step();
    EXPECT_EQ(d.c.GetPhase(), Q::Phase::Holding) << d.log;
}

// The reviewer's repro: a pull on a card lit 33 ms, the hand slips off (6 cm): the trigger held
// 440 ms blocked the gun every frame and took nothing. Now: the gun, the arc closes.
TEST(QuickItemsLeft, PendingPullThatSlipsOffIsTheGun) {
    Rig r;
    r.Squeeze();
    r.Hover(-1);
    r.Step(10);
    r.Hover(2);
    r.Step(3);
    r.Trigger(1.0f);
    r.Step();
    r.Hover(-1); // the hand slips off the card ...
    EXPECT_TRUE(r.Step().block_l2);
    r.in.hand_p.x -= 0.06f; // ... and on, 6 cm
    int blocked = 0;
    for (int i = 0; i < 40; ++i) {
        blocked += r.Step().block_l2 ? 1 : 0;
    }
    EXPECT_EQ(blocked, 0);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_EQ(r.sent.serial, 0u);
}

// An arc opened by a nervous squeeze, the hand fighting but never on a card: closed after
// idle_close2_s (3 s; the hand moving no longer keeps it open). A swing, a leap or a dodge closes
// it at once.
TEST(QuickItemsLeft, ArcOpenedInAFightCloses) {
    Rig r;
    r.Squeeze();
    r.Hover(-1);
    for (int k = 0; k < 5; ++k) { // 2.5 s, the hand moving 5 cm every 0.5 s
        r.in.hand_p.x += (k % 2 == 0) ? 0.05f : -0.05f;
        r.Step(45);
    }
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    r.Step(50);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(r.log.find("CLOSE (idle: no card lit)"), std::string::npos) << r.log;
    // A card lit keeps it open (reading the cards).
    Rig h;
    h.OnCard(0);
    h.Step(400);
    EXPECT_EQ(h.c.GetPhase(), Q::Phase::Open) << h.log;
    // Combat closes it at once; an item in the hand stays.
    Rig c;
    c.Squeeze();
    c.Step(10);
    c.in.combat = true;
    c.in.combat_why = "a swing";
    c.Step();
    c.in.combat = false;
    EXPECT_EQ(c.c.GetPhase(), Q::Phase::Idle);
    EXPECT_NE(c.log.find("CLOSE (a swing)"), std::string::npos) << c.log;
    Rig k;
    k.Take(0);
    k.in.combat = true;
    k.Step();
    EXPECT_EQ(k.c.GetPhase(), Q::Phase::Holding);
}

// The grip (or the trigger) held through a menu, the screen or the headset off (Update not run):
// the first gameplay frame back is no squeeze (the arc opened, at the hip a vial was drawn).
TEST(QuickItemsLeft, HeldThroughAContextSwitchIsNoSqueeze) {
    Rig r;
    r.Step(5);
    r.t += 2.0; // a menu: no Update for 2 s ...
    r.Grip(1.0f); // ... the grip squeezed meanwhile, still squeezed
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.Step(5);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.Grip(0.0f); // let go and squeezed again: the arc
    r.Step();
    r.Grip(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
    Rig h;
    h.Step(5);
    h.t += 2.0;
    h.in.at_hip = true;
    h.Grip(1.0f);
    h.Step();
    EXPECT_FALSE(h.out.draw_vial);
    // The trigger held through it (the presenter's gate: latch and analog read 0, the raw value
    // says held): a grip then is a clench, no arc.
    Rig t;
    t.Step(5);
    t.in.trigger_raw = 0.9f;
    t.Grip(1.0f);
    t.Step();
    EXPECT_EQ(t.c.GetPhase(), Q::Phase::Idle);
    EXPECT_TRUE(Has(t.out, Q::Haptic::Empty)); // felt, not silent
    EXPECT_NE(t.out.log.find("a clench"), std::string::npos) << t.out.log;
}

// An untaken tap outside gameplay is nobody's (left_scheme=2 sends no Square for it).
TEST(QuickItemsLeft, TapOutsideGameplayIsFlagged) {
    Rig r;
    r.in.gameplay = false;
    r.in.arc_tap = true;
    r.Step();
    EXPECT_FALSE(r.out.tap_taken);
    EXPECT_TRUE(r.out.tap_outside);
    r.in.gameplay = true;
    r.Step();
    EXPECT_TRUE(r.out.tap_taken);
    EXPECT_FALSE(r.out.tap_outside);
}

// ---- Scheme 1 is untouched -------------------------------------------------------------------

TEST(QuickItemsLeft, SchemeOneUnchanged) {
    // Y opens, the grip takes, the trigger does not hold a throwable, the grip's release throws.
    Rig r;
    r.cfg.left_scheme = 1;
    r.in.y = true;
    r.Step();
    r.in.y = false;
    r.Step();
    r.Hover(0);
    r.Step(9);
    r.Grip(1.0f);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.Trigger(1.0f);
    r.Step(3);
    r.Trigger(0.0f);
    r.Step(3);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding); // the trigger let go: still held
    r.in.hand_v = {0.0f, 1.0f, -5.0f};
    r.Step(6);
    r.Grip(0.0f);
    r.Step();
    EXPECT_EQ(r.sent.action, Q::Action::Throw);
    r.Result(Q::Outcome::Thrown);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle); // closed, the stack's squeeze takes the next
}

TEST(BodyPouches, FreshGripTakesItemAndGripReleasePutsItBack) {
    Rig r;
    r.cfg.holster_mode = 2;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.out.scene.held_goods, 1200);
    EXPECT_FALSE(r.out.send); // selected slot: pickup consumes nothing
    r.Step(30);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.Grip(0);
    r.Step(6);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.send); // slow release returns it
    r.in.holster_assignment = 0;
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open); // normal layout restored
}
TEST(BodyPouches, HoldingGripBeforeEntryNeverPicksUp) {
    Rig r;
    r.cfg.holster_mode = 1;
    r.Grip(1);
    r.Step();
    r.in.holster_assignment = 2;
    r.Step(30);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.Grip(0);
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
}
TEST(BodyPouches, UnsafeOrEmptyGrabDoesNotOpenMenuOrConsume) {
    for (int scenario = 0; scenario < 9; ++scenario) {
        Rig r;
        r.cfg.holster_mode = 2;
        r.in.holster_assignment = 2;
        if (scenario == 0)
            r.in.snapshot_fresh = false;
        if (scenario == 1)
            r.in.snapshot.alive = false;
        if (scenario == 2)
            r.in.snapshot.busy = true;
        if (scenario == 3)
            r.in.vial_busy = true;
        if (scenario == 4)
            r.in.combat = true;
        if (scenario == 5)
            r.in.hand_valid = false;
        if (scenario == 6)
            r.in.arm_weight = 0.2f;
        if (scenario == 7)
            r.in.snapshot.slot[0].qty = 0;
        if (scenario == 8)
            r.in.snapshot.slot[0].usable = false;
        r.Step();
        r.Grip(1);
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle) << scenario;
        EXPECT_FALSE(r.out.send);
        EXPECT_FALSE(r.out.draw_vial);
        EXPECT_TRUE(Has(r.out, Q::Haptic::Deny) || Has(r.out, Q::Haptic::Empty));
    }
}
TEST(BodyPouches, VialDrawsOnceAndEmptyCaseDenies) {
    Rig r;
    r.cfg.holster_mode = 2;
    r.in.holster_assignment = 1;
    r.in.carried_vials = 3;
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_TRUE(r.out.draw_vial);
    EXPECT_FLOAT_EQ(r.out.vial_grip_release, r.cfg.hold_release);
    r.Step();
    EXPECT_FALSE(r.out.draw_vial);
    EXPECT_LT(r.out.vial_grip_release, 0);
    r.Grip(0);
    r.Step();
    r.in.carried_vials = 0;
    r.Grip(1);
    r.Step();
    EXPECT_FALSE(r.out.draw_vial);
    EXPECT_LT(r.out.vial_grip_release, 0);
    EXPECT_TRUE(Has(r.out, Q::Haptic::Empty));
}
TEST(BodyPouches, SettingsChangeKeepsReleaseOwnership) {
    Rig r;
    r.cfg.holster_mode = 2;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    r.cfg.holster_mode = 0;
    r.cfg.left_scheme = 2;
    r.Step(10);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.Grip(0);
    r.Step(6);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}
TEST(BodyPouches, ContextLossRequiresNewSqueeze) {
    Rig r;
    r.cfg.holster_mode = 2;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    r.in.gameplay = false;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.in.gameplay = true;
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.Grip(0);
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
}
TEST(BodyPouches, PhysicalThrowUsesExistingRequestAndNoFreeStackRegrab) {
    Rig r;
    r.cfg.holster_mode = 1;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    r.in.hand_v = {0, 1, -4};
    r.Step(15);
    r.Grip(0);
    r.Step(5);
    EXPECT_EQ(r.sent.action, Q::Action::Throw);
    r.in.have_result = true;
    r.in.result.serial = r.sent.serial;
    r.in.result.outcome = Q::Outcome::Thrown;
    r.in.holster_assignment = 0;
    r.Step(10);
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
}

TEST(BodyPouches, TrackingLossDuringThrowReturnsItemWithoutUse) {
    Rig r;
    r.cfg.holster_mode = 2;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    r.in.hand_v = {0, 1, -4};
    r.Step(12);
    r.in.hand_valid = false;
    r.Grip(0);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.send);
    r.in.hand_valid = true;
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
    EXPECT_FALSE(r.out.send);
    r.Grip(0);
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
}
TEST(BodyPouches, InitialHeldGripNeedsNeutralBeforeTaking) {
    Rig r;
    r.cfg.holster_mode = 1;
    r.in.holster_assignment = 1;
    r.Grip(1);
    r.Step();
    EXPECT_FALSE(r.out.draw_vial);
    r.Grip(0);
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_TRUE(r.out.draw_vial);
}

TEST(BodyPouches, LeftHipAndChestUseLiveGameSlotsAndAllowSafeReturn) {
    for (int area : {1, 2}) {
        Rig r;
        r.cfg.holster_mode = 1;
        VR::Holsters::Tracker tracker;
        VR::Holsters::Layout rendered{};
        rendered.valid = true;
        rendered.assignment = {1, 2, 3};
        rendered.centre = {VR::Holsters::P3{.16f, 1.f, -.1f},
                           VR::Holsters::P3{-.16f, 1.f, -.1f},
                           VR::Holsters::P3{-.035f, 1.3f, -.12f}};
        const auto layout = tracker.Follow(rendered, rendered.centre[area], true, .1f, 1.f);
        ASSERT_EQ(layout.hover, area);
        r.in.holster_assignment = layout.assignment[layout.hover];
        const int slot = area - 1;
        r.in.snapshot.selected = slot;
        r.Step();
        r.Grip(1);
        r.Step();
        ASSERT_EQ(r.c.GetPhase(), Q::Phase::Holding);
        EXPECT_EQ(r.out.scene.held_slot, slot);
        EXPECT_EQ(r.out.scene.held_goods, r.in.snapshot.slot[slot].goods);
        EXPECT_FALSE(r.out.send); // drawing does not use an item
        r.Grip(0);
        r.Step(8);
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        EXPECT_EQ(r.sent.serial, 0u); // returning consumes nothing

        // The player changes the equipped item in that same game slot.
        r.in.snapshot.slot[slot] = Slots().slot[2];
        r.Step();
        r.Grip(1);
        r.Step();
        EXPECT_EQ(r.out.scene.held_goods, 1110); // Beast Blood Pellet
        r.Grip(0);
        r.Step(8);
        r.in.snapshot.slot[slot].qty = 0;
        r.Grip(1);
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        EXPECT_TRUE(Has(r.out, Q::Haptic::Empty));
        EXPECT_FALSE(r.out.send);
    }
}
TEST(BodyPouches, ChestCanUseANonselectedEquippedQuickItem) {
    Rig r;
    r.cfg.holster_mode = 1;
    r.in.holster_assignment = 3; // default chest -> game quick slot 2
    r.Step();
    r.Grip(1);
    r.Step();
    ASSERT_EQ(r.out.scene.held_goods, 1120); // equipped Blue Elixir
    ASSERT_TRUE(r.out.send);
    EXPECT_EQ(r.sent.action, Q::Action::None); // selection only, not consumption
    EXPECT_EQ(r.sent.slot, 1);
    r.in.snapshot.selected = 1;
    r.Step(20);
    r.Trigger(1);
    r.Step(30);
    EXPECT_EQ(r.sent.action, Q::Action::Use);
    EXPECT_EQ(r.sent.goods, 1120);
}
} // namespace

TEST(BodyPouches, TakingLastItemEmptiesMountAndReturningRestoresIt) {
    for(int slot : {0,1}) {
        Rig r; r.cfg.holster_mode=1; r.in.holster_assignment=slot+2;
        r.in.snapshot.slot[slot].qty=1;
        r.Step();
        EXPECT_TRUE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[slot],slot));
        r.Grip(1);r.Step();
        ASSERT_EQ(r.c.GetPhase(),Q::Phase::Holding);
        EXPECT_FALSE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[slot],slot));
        r.Grip(0);r.Step(8);
        ASSERT_EQ(r.c.GetPhase(),Q::Phase::Idle);
        EXPECT_TRUE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[slot],slot));
        EXPECT_EQ(r.sent.action,Q::Action::None);
    }
}
TEST(BodyPouches, LastThrownItemStaysAbsentWhileWaitingForGame) {
    Rig r; r.cfg.holster_mode=1;r.in.holster_assignment=2;
    r.in.snapshot.slot[0].qty=1;
    r.Step();r.Grip(1);r.Step();
    r.in.hand_v={0,1,-4};r.Step(15);r.Grip(0);r.Step(5);
    ASSERT_EQ(r.sent.action,Q::Action::Throw);
    EXPECT_FALSE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[0],0));
    r.in.snapshot.slot[0].qty=0;
    r.in.have_result=true;r.in.result.serial=r.sent.serial;r.in.result.outcome=Q::Outcome::Thrown;
    r.Step(10);
    EXPECT_FALSE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[0],0));
}
TEST(BodyPouches, SpareItemRemainsVisibleWhileOneIsHeld) {
    Rig r; r.cfg.holster_mode=1;r.in.holster_assignment=2;
    r.in.snapshot.slot[0].qty=2;
    r.Step();r.Grip(1);r.Step();
    ASSERT_EQ(r.c.GetPhase(),Q::Phase::Holding);
    EXPECT_TRUE(Q::ShowStoredItem(r.out.scene,r.in.snapshot.slot[0],0));
}


TEST(WeaponGrabMenu, TwoHandGrabClosesBeforeAnyItemOrMenuInputInBothLayouts) {
    namespace T = VR::TwoHand;
    for (int scheme : {1, 2}) {
        SCOPED_TRACE(scheme);
        Rig r;
        r.cfg.left_scheme = scheme;
        r.in.y = true;
        r.Step();
        r.in.y = false;
        r.Step();
        ASSERT_EQ(r.c.GetPhase(), Q::Phase::Open);
        T::Tracker weapon;
        T::Settings settings;
        T::Input in;
        in.enabled = in.gameplay = in.two_handed = true;
        in.family = 260;
        in.left_tracked = in.right_tracked = true;
        in.handle = {true, {}, {0, 0.14f, 0}, {0, 1, 0}, 0.14f};
        in.left_bone = in.handle.left;
        in.dt = static_cast<float>(Dt);
        in.busy = !Q::WeaponGrabAvailable(r.c.GetPhase());
        T::Output out;
        weapon.Update(settings, in, out);
        in.grip = 1.0f;
        weapon.Update(settings, in, out);
        ASSERT_TRUE(out.grabbed);
        ASSERT_TRUE(out.owns_grip);
        r.in.weapon_grab = out.grabbed;
        r.in.grip = out.owns_grip ? 0.0f : in.grip;
        r.in.two_hand = out.engaged;
        r.in.y = r.in.arc_tap = true; // competing menu-open actions this exact frame
        r.in.at_hip = true;
        r.Hover(0);
        r.Trigger(1.0f); // an item pick must not win over the weapon
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        EXPECT_EQ(r.out.scene.state, Q::SceneState::Closed);
        EXPECT_EQ(r.out.held_goods, -1);
        EXPECT_FALSE(r.out.send);
        EXPECT_FALSE(r.out.draw_vial);
        EXPECT_FALSE(r.out.square_hold);
        EXPECT_FALSE(r.out.square_press);
        EXPECT_TRUE(r.out.block_l2); // the grab's fist is not a gunshot
        EXPECT_TRUE(r.out.tap_taken); // no fall-through to L3
        r.in.weapon_grab = r.in.arc_tap = false;
        r.Step(10); // held Y/trigger cannot reopen or take an item
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
        EXPECT_FALSE(r.out.send);
        r.in.y = false;
        r.Trigger(0.0f);
        r.Step();
        r.in.y = true;
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open); // a later deliberate menu press still works
    }
}

TEST(WeaponGrabMenu, DualBladeHiltClosesArcOnlyOnceItIsActuallyTaken) {
    namespace T = VR::Tricks;
    for (int family : {40, 270}) {
        for (int scheme : {1, 2}) {
            SCOPED_TRACE(family);
            SCOPED_TRACE(scheme);
            Rig r;
            r.cfg.left_scheme = scheme;
            r.in.y = true;
            r.Step();
            r.in.y = false;
            r.Step();
            ASSERT_EQ(r.c.GetPhase(), Q::Phase::Open);
            ASSERT_TRUE(Q::WeaponGrabAvailable(r.c.GetPhase()));
            T::HandTransform weapon;
            T::Settings settings;
            T::Frame in;
            in.dt = static_cast<float>(Dt);
            in.right = {true, {0.2f, 1.25f, -0.35f}, {}, true, true};
            in.left = {true, {0.2f - (T::ReachM + T::NearM) * 0.5f, 1.25f, -0.35f},
                       {}, false, true};
            weapon.Step(in, T::KindOf(family), false, true, settings);
            in.left.grip = true;
            auto out = weapon.Step(in, T::KindOf(family), false, true, settings);
            EXPECT_EQ(out.gesture, T::Gesture::None); // near miss only reserves the squeeze
            ASSERT_TRUE(out.owns_grip);
            r.in.grip = 0.0f;
            r.in.weapon_grab = out.gesture == T::Gesture::Grab;
            r.Step();
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Open);
            in.left.p.x = in.right.p.x - 0.06f;
            out = weapon.Step(in, T::KindOf(family), false, true, settings);
            ASSERT_EQ(out.gesture, T::Gesture::Grab);
            r.in.weapon_grab = out.gesture == T::Gesture::Grab;
            r.Step();
            EXPECT_EQ(r.c.GetPhase(), Q::Phase::Idle);
            EXPECT_EQ(r.out.scene.state, Q::SceneState::Closed);
            EXPECT_FALSE(r.out.send);
            in.left.p.x -= settings.pull_m + 0.01f;
            out = weapon.Step(in, T::KindOf(family), false, true, settings);
            EXPECT_EQ(out.gesture, T::Gesture::Split); // menu close does not cancel the pull
        }
    }
}

TEST(WeaponGrabMenu, HeldItemsAndPendingActionsKeepOwnership) {
    EXPECT_TRUE(Q::WeaponGrabAvailable(Q::Phase::Idle));
    EXPECT_TRUE(Q::WeaponGrabAvailable(Q::Phase::Open));
    for (auto phase : {Q::Phase::Holding, Q::Phase::Committing, Q::Phase::Waiting,
                       Q::Phase::SquareWait}) {
        EXPECT_FALSE(Q::WeaponGrabAvailable(phase));
    }
    Rig r;
    r.Take(0);
    ASSERT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    r.in.weapon_grab = true; // even an erroneous notification must not discard a held item
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
    EXPECT_EQ(r.out.held_goods, 1200);
    EXPECT_FALSE(r.out.send);
}
// holster_mode=1 ("Body slots" only): the grip away from a pouch opens no arc (Y's tap does). With
// y_menu (SteamVR: Y's tap is the game's menu) the grip opens it there too, so the arc is never
// out of reach; at a pouch the grip still takes the pouch's item.
TEST(BodyPouches, YMenuLetsTheGripOpenTheArcInSlotsOnlyMode) {
    for (const bool y_menu : {false, true}) {
        Rig r;
        r.cfg.holster_mode = 1;
        r.cfg.y_menu = y_menu;
        r.Step();
        r.Grip(1);
        r.Step();
        EXPECT_EQ(r.c.GetPhase(), y_menu ? Q::Phase::Open : Q::Phase::Idle) << y_menu;
    }
    Rig r;
    r.cfg.holster_mode = 1;
    r.cfg.y_menu = true;
    r.in.holster_assignment = 2;
    r.Step();
    r.Grip(1);
    r.Step();
    EXPECT_EQ(r.c.GetPhase(), Q::Phase::Holding);
}
