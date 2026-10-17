// SPDX-License-Identifier: GPL-2.0-or-later

// Hold to use (quick_hold_use, the user's 2026-10-06 request): the misc items whose use cannot be
// taken back (the coldbloods, Madman's Knowledge, the NPC bloods, the Hunter's Marks, the bells,
// ...) taken from the quick-item arc stay in the hunter's hand while the button that took them is
// held (left_scheme=2: the trigger; 1: the grip); a ring fills over hold_use_s with a rising
// vibration and full, the item is used (a thump); let go of before, it goes back, nothing used.
// A hit, a dodge, a menu or death cancel it. core/vr/quick_items.h: HoldOf, HoldHum and the
// Controller in both left-hand layouts.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"

namespace {

namespace Q = VR::Quick;

constexpr double Dt = 1.0 / 90.0;
const Q::V3 Rest{-0.2f, 1.1f, -0.3f};

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {80, 1200, 5, 24, -1, true}; // Molotov (thrown)
    s.slot[1] = {81, 1120, 3, 18, -1, true}; // Blue Elixir (drunk at the mouth)
    s.slot[2] = {82, 1510, 9, 20, -1, true}; // Coldblood Dew (held: consume)
    s.slot[3] = {83, 100, 1, 21, -1, true};  // Hunter's Mark (held: rite, the game's own act)
    s.slot[4] = {84, 1500, 2, 22, -1, true}; // Madman's Knowledge (held: consume)
    s.slot[5] = {85, 2000, 1, 23, -1, true}; // Augur of Ebrietas (a hunter tool: at the pull)
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
    int uses = 0;
    int selects = 0;
    int squares = 0;
    int done = 0, putback = 0, deny = 0, use_feel = 0, grab = 0;
    float max_hum = 0.0f, first_hum = -1.0f, last_hum = 0.0f;
    bool hum_rising = true;
    bool l2_free_while_held = false; // block_l2 off while an item was in the hand
    std::string log;
    double t = 100.0;

    explicit Rig(int scheme = 2, bool hold = true) {
        cfg.left_scheme = scheme;
        cfg.hold_use = hold;
        cfg.mouth = true;
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
            c.Update(cfg, in, out);
            if (out.send) {
                sent = out.request;
                uses += sent.action == Q::Action::Use ? 1 : 0;
                selects += sent.action == Q::Action::None ? 1 : 0;
            }
            squares += out.square_press ? 1 : 0;
            done += Count(out, Q::Haptic::HoldDone);
            putback += Count(out, Q::Haptic::PutBack);
            deny += Count(out, Q::Haptic::Deny);
            use_feel += Count(out, Q::Haptic::Use);
            grab += Count(out, Q::Haptic::Grab);
            if (out.hold_hum > 0.0f) {
                if (first_hum < 0.0f) {
                    first_hum = out.hold_hum;
                }
                hum_rising = hum_rising && out.hold_hum >= last_hum - 1e-6f;
                last_hum = out.hold_hum;
                max_hum = std::max(max_hum, out.hold_hum);
            }
            if (out.held_goods >= 0 && !out.block_l2) {
                l2_free_while_held = true;
            }
            log += out.log.empty() ? "" : out.log + "\n";
            in.x_edge = false;
            in.y = false;
            in.dodge = false;
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
        in.hover.slot = -1;
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
        in.result = {sent.serial, o, "", sent.goods, 9, o == Q::Outcome::Used ? 8 : 9};
        Step();
        in.have_result = false;
    }
    [[nodiscard]] Q::Phase Phase() const {
        return c.GetPhase();
    }
};

// Frames for `s` seconds.
int Frames(double s) {
    return static_cast<int>(std::lround(s / Dt));
}

// ---- Which items -------------------------------------------------------------------------------

TEST(QuickHold, Classification) {
    // The blood echoes and the insight: held, the game's use at once.
    for (const int g : {1510, 1511, 1519, 1590, 1594, 1500, 1501, 704, 705,
                        4320, 4323}) {
        EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::Consume);
    }
    // The marks, the bells, the music box, the Shaman Bone Blade: held, then the game's own act.
    for (const int g : {100, 1400, 111, 200, 205, 225, 700, 2090}) {
        EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::Rite);
        EXPECT_TRUE(Q::Classify(g).use == Q::UseKind::Square);
    }
    // Never: eaten / drunk, thrown, the Blood Vial, the other hunter tools, the reversible tools.
    for (const int g : {701, 702, 703, 706, 1100, 1101, 1110, 1120, 2030, 1200, 1201, 1210, 1220, 1230, 1240, 1250,
                        1251, 1260, 1270, 1410, 1000, 2000, 2010, 2020, 2050, 2060, 2070, 2080,
                        2110, 2120, 2130, 2140, 1411, 1412, 120, 900, 901, 800, -1}) {
        EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::None);
    }
    // No item is both held and eaten / drunk or thrown.
    for (const std::int32_t g : Q::QuickUsableGoods) {
        if (Q::HoldOf(g) != Q::HoldKind::None) {
            EXPECT_TRUE(Q::MouthOf(g) == Q::MouthKind::None);
            EXPECT_TRUE(Q::Classify(g).use != Q::UseKind::Throw);
        }
    }
    // The list has no duplicates.
    std::set<std::int32_t> seen(Q::QuickUsableGoods.begin(), Q::QuickUsableGoods.end());
    EXPECT_EQ(seen.size(), Q::QuickUsableGoods.size());
    // The goods dump's goodsUseAnim agrees with the table.
    struct Row {
        int goods, anim;
    };
    for (const Row r : {Row{100, 16}, Row{1400, 16}, Row{111, 15}, Row{200, 14}, Row{205, 14},
                        Row{225, 14}, Row{700, 27}, Row{701, 28}, Row{703, 28}, Row{704, 9},
                        Row{1500, 9}, Row{1510, 9}, Row{1594, 9}, Row{1300, 1}, Row{1310, 1},
                        Row{1330, 5}, Row{1700, 33}, Row{4320, 35}, Row{2090, 25}}) {
        EXPECT_TRUE(Q::HoldAnimAgrees(Q::HoldOf(r.goods), r.anim));
    }
    EXPECT_FALSE(Q::HoldAnimAgrees(Q::HoldKind::Consume, 10));
    EXPECT_FALSE(Q::HoldAnimAgrees(Q::HoldKind::Rite, 9));
}

// The canned uses (shown flat, cinematic_item): the Shaman Bone Blade only, its stab clip the
// behaviour's UseItem_25; the other game-animation items keep their handling.
TEST(QuickHold, CannedIsTheShamanBoneBladeOnly) {
    EXPECT_TRUE(Q::CannedOf(2090));
    EXPECT_EQ(Q::CannedClipOf(2090), 100370);
    EXPECT_EQ(Q::CannedClipOf(2090), Q::UseClipOf(25));
    EXPECT_TRUE(Q::CannedAnimAgrees(2090, 25));
    EXPECT_FALSE(Q::CannedAnimAgrees(2090, 18));
    int canned = 0;
    for (const std::int32_t g : Q::QuickUsableGoods) {
        canned += Q::CannedOf(g) ? 1 : 0;
        if (Q::CannedOf(g)) {
            // Held to use (a stray pull never wastes it), the game's own select + Square.
            EXPECT_TRUE(Q::HoldOf(g) == Q::HoldKind::Rite);
            EXPECT_TRUE(Q::Classify(g).use == Q::UseKind::Square);
        } else {
            EXPECT_EQ(Q::CannedClipOf(g), -1);
            EXPECT_TRUE(Q::CannedAnimAgrees(g, 0));
        }
    }
    EXPECT_EQ(canned, 1);
    for (const int g : {100, 1400, 111, 200, 205, 225, 700, 2000, 2050, 1000, -1}) {
        EXPECT_FALSE(Q::CannedOf(g)); // the marks, the bells, the music box, the other tools
    }
}

// The Shaman Bone Blade: taken from the arc, a stray pull (let go at 0.8 s) puts it back, nothing
// used; held 1.5 s the ring fills (the thump), the slot is selected and the game's Square goes out
// for it: Output::square_goods names it (the presenter arms the flat view from it).
TEST(QuickHold, ShamanBoneBladeHeldThenTheGamesStab) {
    Rig r;
    r.in.snapshot.slot[5] = {85, 2090, 3, 23, -1, true};
    r.Take2(5);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    EXPECT_NE(r.log.find("HOLD to use (rite)"), std::string::npos);
    EXPECT_EQ(r.squares, 0); // not at the pull any more
    r.Step(Frames(0.8));
    r.Trigger(0.0f);
    r.Step();
    EXPECT_TRUE(r.Phase() == Q::Phase::Open); // a stray pull: back on the arc
    EXPECT_EQ(r.squares, 0);
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.putback, 1);

    r.Hover(5);
    r.Step(20);
    r.Trigger(1.0f);
    r.Step();
    const int selects = r.selects;
    r.Step(Frames(1.5) + 3);
    EXPECT_EQ(r.done, 1);
    EXPECT_EQ(r.uses, 0); // never UseGoods: the game's own stab
    EXPECT_EQ(r.selects, selects + 1);
    EXPECT_EQ(r.squares, 0);
    r.in.snapshot.selected = 5;
    r.Step();
    EXPECT_EQ(r.squares, 1);
    EXPECT_EQ(r.out.square_goods, 2090);
    EXPECT_NE(r.log.find("a canned use"), std::string::npos);
    r.Step();
    EXPECT_EQ(r.out.square_goods, -1); // one frame only
}

// A mark's Square names the mark (not canned: it stays in VR).
TEST(QuickHold, MarkSquareIsNotCanned) {
    Rig r;
    r.Take2(3);
    r.Step(Frames(1.5) + 3);
    r.in.snapshot.selected = 3;
    r.Step();
    EXPECT_EQ(r.squares, 1);
    EXPECT_EQ(r.out.square_goods, 100);
    EXPECT_FALSE(Q::CannedOf(r.out.square_goods));
    EXPECT_EQ(r.log.find("a canned use"), std::string::npos);
}

TEST(QuickHold, HumRisesGentleToFirm) {
    EXPECT_NEAR(Q::HoldHum(0.0f), 0.22f, 1e-6f);
    EXPECT_NEAR(Q::HoldHum(1.0f), 0.62f, 1e-6f);
    EXPECT_NEAR(Q::HoldHum(2.0f), 0.62f, 1e-6f);
    float last = 0.0f;
    for (int i = 0; i <= 20; ++i) {
        const float h = Q::HoldHum(static_cast<float>(i) / 20.0f);
        EXPECT_GE(h, last);
        last = h;
    }
    // Most of the rise near the end: the second half rises three times the first.
    EXPECT_GT(Q::HoldHum(1.0f) - Q::HoldHum(0.5f), 2.9f * (Q::HoldHum(0.5f) - Q::HoldHum(0.0f)));
}

// ---- left_scheme=2: the trigger holds --------------------------------------------------------

TEST(QuickHold, ColdbloodHeldFullIsUsed) {
    Rig r;
    r.Take2(2);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    EXPECT_EQ(r.uses, 0);
    EXPECT_NE(r.log.find("HOLD to use (consume)"), std::string::npos);
    // The ring fills and the hum rises; nothing is used before it is full.
    r.Step(Frames(1.5) - 3);
    EXPECT_EQ(r.uses, 0);
    EXPECT_GT(r.out.scene.hold_from_ns, 0);
    EXPECT_NEAR(r.out.scene.hold_s, 1.5f, 1e-6f);
    EXPECT_GT(r.out.hold_progress, 0.95f);
    EXPECT_LT(r.out.hold_progress, 1.0f);
    EXPECT_NEAR(r.first_hum, 0.22f, 0.01f);
    EXPECT_TRUE(r.hum_rising);
    EXPECT_GT(r.max_hum, 0.58f);
    EXPECT_FALSE(r.l2_free_while_held); // the gun never fires while it is in the hand
    r.Step(6);
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.sent.goods, 1510);
    EXPECT_EQ(r.done, 1); // the thump
    EXPECT_EQ(r.out.hold_hum, 0.0f);
    EXPECT_GT(r.out.scene.hold_done_ns, 0); // the flash
    EXPECT_EQ(r.out.scene.hold_from_ns, 0); // no ring once full
    EXPECT_TRUE(r.Phase() == Q::Phase::Waiting);
    EXPECT_NE(r.log.find("HOLD full"), std::string::npos);
    r.Result(Q::Outcome::Used);
    EXPECT_EQ(r.use_feel, 0); // the thump was its feel
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_FALSE(r.l2_free_while_held);
    EXPECT_TRUE(r.out.block_l2); // the trigger still held is not the gun until let go
    r.Trigger(0.0f);
    r.Step(2);
    EXPECT_FALSE(r.out.block_l2);
}

// The mock runs of 2026-10-06: a drink at the mouth with the trigger still held, then the game
// hitched (a frame not immersive: no gameplay) and the pistol fired as the frame came back. The
// trigger held since the use stays blocked through frames out of gameplay, until let go.
TEST(QuickHold, AHitchAfterAUseNeverFreesTheHeldTrigger) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.6));
    EXPECT_TRUE(r.Phase() == Q::Phase::Waiting);
    r.Result(Q::Outcome::Used);
    EXPECT_TRUE(r.out.block_l2);
    r.in.gameplay = false; // the hitch
    r.Step(3);
    EXPECT_TRUE(r.out.block_l2);
    r.in.gameplay = true;
    r.Step(2);
    EXPECT_TRUE(r.out.block_l2);
    r.Trigger(0.0f);
    r.Step(2);
    EXPECT_FALSE(r.out.block_l2);
    // Never held, nothing to block: a frame out of gameplay leaves the gun alone.
    r.in.gameplay = false;
    r.Step(2);
    EXPECT_FALSE(r.out.block_l2);
}

TEST(QuickHold, ReleasedEarlyGoesBackUnused) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.8));
    EXPECT_GT(r.out.hold_progress, 0.45f);
    r.Trigger(0.0f);
    r.Step();
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.done, 0);
    EXPECT_EQ(r.putback, 1); // the soft put-back tick
    EXPECT_TRUE(r.Phase() == Q::Phase::Open); // back on the arc
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_EQ(r.out.scene.hold_from_ns, 0);
    EXPECT_EQ(r.out.hold_hum, 0.0f);
    EXPECT_NE(r.log.find("before the ring filled"), std::string::npos);
    r.Step(Frames(2.0));
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, GettingHitCancels) {
    Rig r;
    r.Take2(4); // Madman's Knowledge
    r.Step(Frames(0.7));
    r.in.arm_weight = 0.3f; // a hit's stagger: the game takes the arm
    r.Step(Frames(0.15));
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_EQ(r.putback, 1);
    EXPECT_NE(r.log.find("HOLD cancelled (the game took the arm"), std::string::npos);
    r.in.arm_weight = 1.0f;
    r.Step(Frames(2.0)); // the trigger still held: nothing more
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, AOneFrameArmGlitchDoesNotCancel) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.5));
    r.in.arm_weight = 0.5f;
    r.Step(); // 11 ms: under hold_arm_grace_s
    r.in.arm_weight = 1.0f;
    r.Step(Frames(1.2));
    EXPECT_EQ(r.uses, 1);
}

TEST(QuickHold, AHitchIsNoHit) {
    // A quarter-second hitch: the next frame comes 0.3 s later with the body's state stale (the
    // presenter reads the arm 0 and the body off then), then all is well again: no cancel.
    Rig r;
    r.Take2(2);
    r.Step(Frames(0.6));
    r.t += 0.3;
    r.in.t = r.t;
    r.in.dt = 0.3f;
    r.in.arm_weight = 0.0f;
    r.in.body = false;
    r.c.Update(r.cfg, r.in, r.out); // (Step would make it an ordinary 11 ms frame)
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    // Even read as yours-not (the body fresh, the arm 0) for one long frame: no hit (3 frames).
    r.t += 0.3;
    r.in.t = r.t;
    r.in.body = true;
    r.c.Update(r.cfg, r.in, r.out);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.in.arm_weight = 1.0f;
    r.in.body = true;
    r.Step(2);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    EXPECT_EQ(r.putback, 0);
    r.Step(Frames(0.7));
    EXPECT_EQ(r.uses, 1);
}

TEST(QuickHold, DodgeCancels) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.0));
    r.in.dodge = true;
    r.Step();
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_EQ(r.putback, 1);
    r.Step(Frames(1.0));
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, MenuCancelsSilently) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.0));
    r.in.gameplay = false;
    r.Step();
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_EQ(r.out.hold_hum, 0.0f);
    r.in.gameplay = true;
    r.Step(Frames(1.0));
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, DeathCancels) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.0));
    r.in.snapshot.alive = false;
    r.Step();
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    r.Step(Frames(1.0));
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, MarkKeepsTheGamesOwnAct) {
    Rig r;
    r.Take2(3); // Hunter's Mark
    EXPECT_NE(r.log.find("HOLD to use (rite)"), std::string::npos);
    const int selects = r.selects;
    r.Step(Frames(1.5) + 3);
    EXPECT_EQ(r.uses, 0); // never UseGoods
    EXPECT_EQ(r.done, 1);
    EXPECT_EQ(r.selects, selects + 1); // the slot selected for the game's own use ...
    r.in.snapshot.selected = 3;
    r.Step();
    EXPECT_EQ(r.squares, 1); // ... and Square
}

TEST(QuickHold, CannotBeUsedNowIsADenyAtTheTake) {
    Rig r;
    r.in.snapshot.slot[2].usable = false;
    r.Take2(2);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
    EXPECT_EQ(r.deny, 1);
    r.Step(Frames(2.0));
    EXPECT_EQ(r.uses, 0);
}

TEST(QuickHold, RefusedWhenFullGoesBack) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.0));
    r.in.snapshot.slot[2].usable = false; // the game says no meanwhile
    r.Step(Frames(0.6));
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.deny, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
}

TEST(QuickHold, TheGameRefusingTheUseGoesBack) {
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.5) + 3);
    EXPECT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Denied);
    EXPECT_EQ(r.deny, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
    r.Step(Frames(2.0));
    EXPECT_EQ(r.uses, 1); // never again by itself
}

TEST(QuickHold, CooldownWaitsAtFull) {
    Rig r;
    r.cfg.use_cooldown_s = 3.0f;
    r.Take2(2);
    r.Step(Frames(1.5) + 3);
    r.Result(Q::Outcome::Used);
    r.Trigger(0.0f);
    r.Step(2);
    r.in.snapshot.slot[2].qty = 8;
    r.Take2(2); // at once again: 0.3 s later
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.Step(Frames(1.6));
    EXPECT_EQ(r.uses, 1); // full, waiting out the cooldown
    EXPECT_NEAR(r.out.hold_progress, 1.0f, 1e-6f);
    r.Step(Frames(1.5));
    EXPECT_EQ(r.uses, 2);
}

TEST(QuickHold, FullWhileTheGameHasTheArmWaitsForIt) {
    // The ring fills as a hit's stagger begins: nothing is used through the game's hold on the
    // arm; back within the grace (a glitch), it is used then ...
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.5) - 2);
    r.in.arm_weight = 0.3f;
    r.Step(4); // full now, the arm the game's for 44 ms
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.done, 0);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.in.arm_weight = 1.0f;
    r.Step();
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.done, 1);
    // ... and kept past it, the hold is cancelled, nothing used.
    Rig h;
    h.Take2(4);
    h.Step(Frames(1.5) - 2);
    h.in.arm_weight = 0.3f;
    h.Step(Frames(0.2));
    EXPECT_EQ(h.uses, 0);
    EXPECT_EQ(h.done, 0);
    EXPECT_EQ(h.putback, 1);
    EXPECT_TRUE(h.Phase() == Q::Phase::Idle);
}

TEST(QuickHold, TheFlashStaysWithTheItemUsed) {
    // Used: the flash (Scene::hold_done_ns) ... the next item taken shows its own ring from 0,
    // never the last one's flash (the ring would show full on it).
    Rig r;
    r.Take2(2);
    r.Step(Frames(1.5) + 3);
    r.Result(Q::Outcome::Used);
    EXPECT_GT(r.out.scene.hold_done_ns, 0);
    r.Trigger(0.0f);
    r.Step(2);
    EXPECT_GT(r.out.scene.hold_done_ns, 0); // (drawn where the item last was)
    r.Take2(4);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    EXPECT_GT(r.out.scene.hold_from_ns, 0);
    EXPECT_EQ(r.out.scene.hold_done_ns, 0);
    r.Trigger(0.0f);
    r.Step(); // let go: back on the arc
    EXPECT_TRUE(r.Phase() == Q::Phase::Open);
    r.Hover(0);
    r.Step(20);
    r.Trigger(1.0f);
    r.Step(); // a Molotov: no ring, no flash
    EXPECT_EQ(r.out.held_goods, 1200);
    EXPECT_EQ(r.out.scene.hold_from_ns, 0);
    EXPECT_EQ(r.out.scene.hold_done_ns, 0);
}

TEST(QuickHold, MouthAndThrowsAndToolsUnchanged) {
    {
        Rig r;
        r.Take2(1); // Blue Elixir: waits for the mouth, no ring
        r.Step(Frames(2.0));
        EXPECT_EQ(r.uses, 0);
        EXPECT_EQ(r.out.scene.hold_from_ns, 0);
        EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    }
    {
        Rig r;
        r.Take2(0); // Molotov: held to throw, no ring
        r.Step(Frames(2.0));
        EXPECT_EQ(r.uses, 0);
        EXPECT_EQ(r.out.hold_progress, -1.0f);
        EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    }
    {
        Rig r;
        r.Take2(5); // the Augur: at the pull, the game's own animation
        EXPECT_EQ(r.done, 0);
        EXPECT_TRUE(r.Phase() == Q::Phase::SquareWait || r.Phase() == Q::Phase::Idle);
    }
}

TEST(QuickHold, PapersAreTheWipesWhenItIsOn) {
    // quick_use_wipe on: the Fire Paper waits for the blade, no ring.
    {
        Rig r;
        r.cfg.wipe = true;
        r.in.snapshot.slot[5] = {85, 1300, 3, 23, -1, true};
        // A blade in the hunter's hand, the paper far from it (the render-camera hook's sample;
        // with none the paper falls back to the ring after 0.5 s: test_quick_wipe.cpp).
        r.in.wipe = Q::WipeMeasure({-0.2f, 1.1f, -0.3f}, {0.25f, 1.1f, -0.35f},
                                   {0.25f, 1.1f, -0.85f});
        r.in.wipe.goods = 1300;
        r.in.wipe.arms = true;
        r.in.wipe_fresh = true;
        r.Take2(5);
        r.Step(Frames(2.0));
        EXPECT_EQ(r.uses, 0);
        EXPECT_EQ(r.out.scene.hold_from_ns, 0);
        EXPECT_EQ(r.out.hold_progress, -1.0f);
        EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    }
    // ... off: held to use like the rest.
    {
        Rig r;
        r.cfg.wipe = false;
        r.in.snapshot.slot[5] = {85, 1300, 3, 23, -1, true};
        r.Take2(5);
        r.Step(Frames(1.5) + 3);
        EXPECT_EQ(r.uses, 1);
        EXPECT_EQ(r.done, 1);
    }
}

TEST(QuickHold, OffUsesAtThePull) {
    Rig r(2, false);
    r.Take2(2);
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.out.scene.hold_from_ns, 0);
}

TEST(QuickHold, LengthIsTheSetting) {
    Rig r;
    r.cfg.hold_use_s = 2.0f;
    r.Take2(2);
    r.Step(Frames(1.9));
    EXPECT_EQ(r.uses, 0);
    r.Step(Frames(0.15));
    EXPECT_EQ(r.uses, 1);
}

// ---- left_scheme=1: the grip holds ----------------------------------------------------------

TEST(QuickHold, SchemeOneGripHeldFullIsUsed) {
    Rig r(1);
    r.Take1(2);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding);
    r.Step(Frames(0.4));
    // The trigger meanwhile: no use, no gun.
    r.Trigger(1.0f);
    r.Step(Frames(0.3));
    r.Trigger(0.0f);
    EXPECT_EQ(r.uses, 0);
    EXPECT_FALSE(r.l2_free_while_held);
    r.Step(Frames(0.85));
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.done, 1);
    r.Result(Q::Outcome::Used);
    EXPECT_EQ(r.use_feel, 0);
}

TEST(QuickHold, SchemeOneGripReleasedEarlyPutsAway) {
    Rig r(1);
    r.Take1(4);
    r.Step(Frames(0.8));
    r.Grip(0.0f);
    r.Step();
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.putback, 1);
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_NE(r.log.find("before the ring filled"), std::string::npos);
}

TEST(QuickHold, SchemeOneRefusedStaysSpentUntilLetGo) {
    Rig r(1);
    r.Take1(2);
    r.Step(Frames(1.5) + 3);
    EXPECT_EQ(r.uses, 1);
    r.Result(Q::Outcome::Denied);
    EXPECT_TRUE(r.Phase() == Q::Phase::Holding); // still in the hand ...
    EXPECT_EQ(r.out.scene.hold_from_ns, 0);      // ... no ring
    r.Step(Frames(2.0));
    EXPECT_EQ(r.uses, 1); // never again by itself
    r.Grip(0.0f);
    r.Step();
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
}

TEST(QuickHold, SchemeOneHitCancels) {
    Rig r(1);
    r.Take1(2);
    r.Step(Frames(0.6));
    r.in.arm_weight = 0.2f;
    r.Step(Frames(0.2));
    EXPECT_TRUE(r.Phase() == Q::Phase::Idle);
    EXPECT_EQ(r.putback, 1);
    r.in.arm_weight = 1.0f;
    r.Step(Frames(1.5));
    EXPECT_EQ(r.uses, 0);
}

} // namespace
