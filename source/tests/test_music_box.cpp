// SPDX-License-Identifier: GPL-2.0-or-later

// The Tiny Music Box played by hand (quick_music_box_crank, the user's 2026-10-06 request): the
// box in the left hand frees the right one, whose grip takes the crank's knob within 6 cm and
// turns it; a full turn plays the box (the game's UseGoods, its tune), a ratchet tick per quarter
// turn; letting go of the box brings the right weapon back at once. core/vr/music_box.h (the
// geometry, the crank, the restore rule) and the quick items' Controller (core/vr/quick_items.h)
// in both left-hand layouts.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "core/vr/music_box.h"
#include "core/vr/music_box_grip.h"
#include "core/vr/music_box_mesh.h"
#include "core/vr/music_box_texture.h"
#include "c0000_test_skeleton.h"
#include "core/vr/quick_items.h"

namespace {

namespace MB = VR::MusicBox;
namespace Q = VR::Quick;

constexpr double Dt = 1.0 / 90.0;

MB::Frame Box() {
    return MB::FrameOf(MB::Geometry{}, {-0.15f, 1.10f, -0.35f}, {});
}

// The right hand at `angle` round the crank's axis, `radius` off it, in the knob's plane.
MB::V3 HandAt(const MB::Frame& f, float angle, float radius = MB::Geometry{}.arm_m) {
    const MB::Geometry g{};
    return f.hub + f.u * (std::cos(angle) * radius) + f.v * (std::sin(angle) * radius) +
           f.axis * (0.5f * g.knob_out_m);
}

// Turns the grabbed crank by `turns` (negative: the other way) over `seconds`, from `from` rad.
struct Turned {
    int ticks{0};
    int plays{0};
    float progress{0.0f};
};
Turned Turn(MB::Crank& c, const MB::Frame& f, double& t, float from, float turns, double seconds,
            const MB::Config& cfg = {}) {
    Turned r{};
    const int frames = static_cast<int>(std::lround(seconds / Dt));
    const MB::Geometry g{};
    for (int i = 1; i <= frames; ++i) {
        t += Dt;
        const float a = from + MB::TwoPi * turns * static_cast<float>(i) / frames;
        const MB::Step s = c.Update(cfg, g, f, HandAt(f, a), t);
        r.ticks += s.ticks;
        r.plays += s.play ? 1 : 0;
        r.progress = s.progress;
    }
    return r;
}

TEST(MusicBox, GeometryAndAngles) {
    const MB::Geometry g{};
    const MB::Frame f = Box();
    // The box rests on the palm: up is grip +X, crank axis -Y, quarter turn +Z.
    EXPECT_NEAR(f.axis.y, -1.0f, 1e-5f);
    EXPECT_NEAR(f.u.x, 1.0f, 1e-5f);
    EXPECT_NEAR(f.v.z, 1.0f, 1e-5f);
    const auto up_from_q = Q::Rotate(f.q, {0,1,0});
    EXPECT_NEAR(Q::Dot(up_from_q, f.u), 1.0f, 1e-5f);
    EXPECT_NEAR(Q::Dot(f.axis, f.u), 0.0f, 1e-5f);
    // The hub on the box's +X side, the knob an arm away and out along the axis.
    EXPECT_NEAR(f.box.y - f.hub.y, 0.5f * g.width_m + g.hub_out_m, 1e-5f);
    const MB::V3 k0 = MB::KnobAt(g, f, 0.0f);
    EXPECT_NEAR(k0.x - f.hub.x, g.arm_m, 1e-5f);
    EXPECT_NEAR(f.hub.y - k0.y, 0.5f * g.knob_out_m, 1e-5f);
    for (const float a : {-3.0f, -1.5f, 0.0f, 0.7f, 2.5f}) {
        float r = 0.0f;
        EXPECT_NEAR(MB::AngleOf(f, MB::KnobAt(g, f, a), &r), a, 1e-4f);
        EXPECT_NEAR(r, g.arm_m, 1e-5f);
    }
    // A turned left hand turns the crank's frame with it.
    const float h = std::sqrt(0.5f);
    const MB::Frame turned = MB::FrameOf(g, {}, {0.0f, h, 0.0f, h}); // 90 degrees about +Y
    EXPECT_NEAR(turned.u.z, -1.0f, 1e-4f);
    EXPECT_NEAR(MB::WrapPi(3.5f), 3.5f - MB::TwoPi, 1e-5f);
    EXPECT_NEAR(MB::WrapPi(-3.5f), -3.5f + MB::TwoPi, 1e-5f);
}

TEST(MusicBox, GrabZone) {
    const MB::Config cfg{};
    const MB::Geometry g{};
    const MB::Frame f = Box();
    const MB::V3 knob = MB::KnobAt(g, f, 0.0f);
    {
        MB::Crank c{};
        float d = 0.0f;
        EXPECT_TRUE(c.TryGrab(cfg, g, f, knob + MB::V3{0.0f, 0.0f, 0.055f}, &d));
        // (5.5 cm from the knob, nearer its circle: the distance that counts.)
        EXPECT_NEAR(d, std::hypot(g.arm_m, 0.055f) - g.arm_m, 1e-4f);
        EXPECT_TRUE(c.Grabbed());
    }
    {
        // The knob left at the bottom (against the left hand): taken from the top of its circle,
        // 6 cm away from it, as from anywhere near the circle.
        MB::Crank c{};
        EXPECT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, MB::Pi - 0.2f)));
        c.Release();
        float d = 0.0f;
        EXPECT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 0.0f, 0.05f) + f.axis * 0.02f, &d));
        EXPECT_NEAR(d, std::hypot(0.05f - g.arm_m, 0.02f), 1e-4f);
        EXPECT_NEAR(c.Angle(), 0.0f, 1e-4f);
        c.Release();
        // Too far from the circle (7 cm out along the axis): no.
        EXPECT_FALSE(c.TryGrab(cfg, g, f, HandAt(f, 0.0f) + f.axis * 0.07f));
    }
    {
        MB::Crank c{};
        EXPECT_FALSE(c.TryGrab(cfg, g, f, knob + MB::V3{0.065f, 0.0f, 0.0f}));
        EXPECT_FALSE(c.Grabbed());
    }
    {
        MB::Crank c{};
        EXPECT_FALSE(c.TryGrab(cfg, g, MB::Frame{}, knob)); // no box pose
    }
    {
        // Taken from the side of the circle, the knob jumps to the hand's angle.
        MB::Crank c{};
        EXPECT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 1.0f)));
        EXPECT_NEAR(c.Angle(), 1.0f, 1e-4f);
    }
}

TEST(MusicBox, OneTurnPlaysWithAQuarterTick) {
    for (const float way : {1.0f, -1.0f}) {
        const MB::Config cfg{};
        const MB::Frame f = Box();
        MB::Crank c{};
        double t = 10.0;
        ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.0f)));
        // Three quarters: three ticks, no play.
        Turned r = Turn(c, f, t, 0.0f, 0.75f * way, 0.8);
        EXPECT_EQ(r.ticks, 3) << way;
        EXPECT_EQ(r.plays, 0) << way;
        EXPECT_NEAR(r.progress, 0.75f, 0.03f);
        EXPECT_EQ(c.Direction(), way > 0.0f ? 1 : -1);
        // The last quarter: the play.
        r = Turn(c, f, t, 0.75f * MB::TwoPi * way, 0.3f * way, 0.4);
        EXPECT_EQ(r.plays, 1) << way;
        EXPECT_EQ(c.Plays(), 1);
        EXPECT_GE(r.ticks, 1);
    }
}

TEST(MusicBox, WigglingNeverPlays) {
    const MB::Config cfg{};
    const MB::Frame f = Box();
    MB::Crank c{};
    double t = 10.0;
    ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.0f)));
    int plays = 0;
    for (int i = 0; i < 20; ++i) {
        plays += Turn(c, f, t, 0.0f, 0.06f, 0.15).plays;  // +22 degrees
        plays += Turn(c, f, t, 0.06f * MB::TwoPi, -0.06f, 0.15).plays; // back
    }
    EXPECT_EQ(plays, 0);
    EXPECT_LT(c.Progress(cfg), 0.25f);
}

TEST(MusicBox, RatchetKeepsWhatWasTurned) {
    const MB::Config cfg{};
    const MB::Frame f = Box();
    MB::Crank c{};
    double t = 10.0;
    ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.0f)));
    Turn(c, f, t, 0.0f, 0.5f, 0.5);
    const float half = c.Progress(cfg);
    EXPECT_NEAR(half, 0.5f, 0.03f);
    // Back a quarter (free), then on: the play comes after the other half.
    Turn(c, f, t, 0.5f * MB::TwoPi, -0.25f, 0.3);
    EXPECT_NEAR(c.Progress(cfg), half, 1e-4f);
    const Turned r = Turn(c, f, t, 0.25f * MB::TwoPi, 0.55f, 0.6);
    EXPECT_EQ(r.plays, 1);
    // Let go and taken again: what was turned stays (a new box starts at 0).
    Turn(c, f, t, 0.8f * MB::TwoPi, 0.3f, 0.3);
    const float kept = c.Progress(cfg);
    c.Release();
    EXPECT_FALSE(c.Grabbed());
    ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.1f)));
    EXPECT_NEAR(c.Progress(cfg), kept, 1e-4f);
    c.Reset();
    EXPECT_EQ(c.Progress(cfg), 0.0f);
}

TEST(MusicBox, KeepTurningReplaysOnceTheTuneIsOver) {
    MB::Config cfg{};
    cfg.replay_s = 3.0f;
    const MB::Frame f = Box();
    MB::Crank c{};
    double t = 10.0;
    ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.0f)));
    // Two turns a second: one play at the first turn, none for the next 3 s however fast.
    Turned r = Turn(c, f, t, 0.0f, 5.0f, 2.5, cfg);
    EXPECT_EQ(r.plays, 1);
    // On turning: the next play once the tune is over (a turn already waits), and so on.
    r = Turn(c, f, t, 0.0f, 3.0f, 1.5, cfg);
    EXPECT_EQ(r.plays, 1);
    EXPECT_EQ(c.Plays(), 2);
    // Stopped: nothing more.
    for (int i = 0; i < 400; ++i) {
        t += Dt;
        EXPECT_FALSE(c.Update(cfg, MB::Geometry{}, f, HandAt(f, 0.0f), t).play);
    }
}

TEST(MusicBox, ARefusedPlayCostsNoReplayWait) {
    const MB::Config cfg{};
    const MB::Frame f = Box();
    MB::Crank c{};
    double t = 10.0;
    ASSERT_TRUE(c.TryGrab(cfg, MB::Geometry{}, f, HandAt(f, 0.0f)));
    Turned r = Turn(c, f, t, 0.0f, 1.05f, 0.8, cfg);
    ASSERT_EQ(r.plays, 1);
    // The game refused it (a roll): the next full turn plays at once, not after replay_s.
    c.Refused();
    c.Refused(); // (twice is once)
    EXPECT_EQ(c.Plays(), 0);
    r = Turn(c, f, t, 1.05f * MB::TwoPi, 1.05f, 0.8, cfg);
    EXPECT_EQ(r.plays, 1);
    EXPECT_EQ(c.Plays(), 1);
    // Played: the next one waits for the tune again.
    r = Turn(c, f, t, 2.1f * MB::TwoPi, 2.0f, 1.5, cfg);
    EXPECT_EQ(r.plays, 0);
}

TEST(MusicBox, TrackingJumpsAndTheAxis) {
    const MB::Config cfg{};
    const MB::Geometry g{};
    const MB::Frame f = Box();
    MB::Crank c{};
    double t = 10.0;
    ASSERT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 0.0f)));
    // A jump of half a turn in one frame is not counted.
    t += Dt;
    const MB::Step s = c.Update(cfg, g, f, HandAt(f, 3.0f), t);
    EXPECT_TRUE(s.jumped);
    EXPECT_EQ(c.Progress(cfg), 0.0f);
    // On the axis: no angle, nothing turns.
    const float before = c.Angle();
    t += Dt;
    c.Update(cfg, g, f, HandAt(f, 1.0f, 0.002f), t);
    EXPECT_EQ(c.Angle(), before);
    // Pulled far from the crank: let go.
    t += Dt;
    c.Update(cfg, g, f, f.hub + MB::V3{0.0f, 0.3f, 0.0f}, t);
    EXPECT_FALSE(c.Grabbed());
    EXPECT_TRUE(c.TakeReleasedFar());
    EXPECT_FALSE(c.TakeReleasedFar());
}

TEST(MusicBox, RestoreRuleAndSnap) {
    EXPECT_TRUE(MB::RightHandFree({true, true, true}));
    EXPECT_FALSE(MB::RightHandFree({false, true, true})); // put back
    EXPECT_FALSE(MB::RightHandFree({true, false, true})); // a menu, the screen, a cinematic
    EXPECT_FALSE(MB::RightHandFree({true, true, false})); // death
    float w = 0.0f;
    for (int i = 0; i < 4; ++i) {
        w = MB::SnapStep(w, true, 0.011f, 0.08f);
    }
    EXPECT_GT(w, 0.5f);
    EXPECT_LT(w, 1.0f);
    for (int i = 0; i < 10; ++i) {
        w = MB::SnapStep(w, true, 0.011f, 0.08f);
    }
    EXPECT_EQ(w, 1.0f);
    for (int i = 0; i < 10; ++i) {
        w = MB::SnapStep(w, false, 0.011f, 0.08f);
    }
    EXPECT_EQ(w, 0.0f);
    // The scene's freshness.
    MB::Scene s{};
    s.active = true;
    s.steady_ns = 1'000'000'000;
    EXPECT_TRUE(MB::ActiveAt(s, s.steady_ns + 100'000'000));
    EXPECT_FALSE(MB::ActiveAt(s, s.steady_ns + MB::StaleNs + 1));
    s.active = false;
    EXPECT_FALSE(MB::ActiveAt(s, s.steady_ns));
    // The tune: the clip's own two sounds.
    EXPECT_EQ(MB::Tune().count, 2);
    EXPECT_EQ(MB::Tune().sound[0].id, 100171);
    EXPECT_EQ(MB::Tune().sound[1].id, 100170);
}

// ---- The quick items' Controller --------------------------------------------------------------

Q::Snapshot Slots() {
    Q::Snapshot s{};
    s.valid = true;
    s.slot[0] = {90, 700, 1, 30, -1, true};  // Tiny Music Box
    s.slot[1] = {91, 1510, 9, 20, -1, true}; // Coldblood Dew (held: the ring)
    s.selected = 0;
    return s;
}

struct Rig {
    Q::Controller c;
    Q::Config cfg{};
    Q::Input in{};
    Q::Output out{};
    Q::Request sent{};
    int uses = 0;
    int squares = 0;
    bool ring = false; // a hold ring was shown
    double t = 100.0;

    explicit Rig(int scheme = 2, bool crank = true) {
        cfg.left_scheme = scheme;
        cfg.hold_use = true;
        cfg.crank = crank;
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
                uses += sent.action == Q::Action::Use ? 1 : 0;
            }
            squares += out.square_press ? 1 : 0;
            ring = ring || out.hold_progress >= 0.0f;
            in.crank_play = false;
            in.y = false;
        }
        return out;
    }
    void Trigger(float v) {
        in.trigger = v;
        in.trigger_raw = v;
        in.trigger_latch = in.trigger_latch ? v > 0.25f : v >= 0.45f;
    }
    void Hover(int slot) {
        in.hover_fresh = true;
        in.hover.open_serial = c.OpenSerial();
        in.hover.placed = true;
        in.hover.slot = static_cast<std::int8_t>(slot);
    }
    void Take2(int slot) {
        in.grip = 1.0f;
        Step();
        in.grip = 0.0f;
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
        in.grip = 1.0f;
        Step();
        in.hover.slot = -1;
    }
};

TEST(MusicBoxController, Scheme2TakePlayDrop) {
    Rig r(2);
    r.Take2(0);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_TRUE(r.out.crank_box);
    EXPECT_EQ(r.out.held_goods, 700);
    EXPECT_TRUE(r.out.block_l2); // the trigger holds the box: never the gun
    // Held 3 s: no ring, no use, no animation (the crank plays it).
    r.Step(270);
    EXPECT_FALSE(r.ring);
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.squares, 0);
    EXPECT_TRUE(r.out.crank_box);
    // The crank went round: the game's use at once, the box stays in the hand.
    r.in.crank_play = true;
    r.Step();
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.sent.goods, 700);
    EXPECT_EQ(r.sent.crank, 1);
    EXPECT_EQ(r.sent.dmy, 17);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Holding);
    EXPECT_TRUE(r.out.crank_box);
    // Its answer.
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Used, "", 700, 1, 1};
    r.Step();
    EXPECT_EQ(r.out.crank_answer, 1);
    r.in.have_result = false;
    r.Step();
    EXPECT_EQ(r.out.crank_answer, 0);
    // Played again later.
    r.Step(60);
    r.in.crank_play = true;
    r.Step();
    EXPECT_EQ(r.uses, 2);
    // Let go of the trigger: back on the arc, the right hand's weapon back on this very frame.
    r.Trigger(0.0f);
    r.Step();
    EXPECT_FALSE(r.out.crank_box);
    EXPECT_EQ(r.out.held_goods, -1);
    EXPECT_EQ(r.squares, 0);
}

TEST(MusicBoxController, RefusedPlayIsADeny) {
    Rig r(2);
    r.Take2(0);
    r.in.snapshot.slot[0].usable = false;
    r.in.crank_play = true;
    r.Step();
    EXPECT_EQ(r.uses, 0);
    EXPECT_EQ(r.out.crank_answer, -1);
    EXPECT_TRUE(r.out.crank_box); // still in the hand
    // The game's refusal of a sent play.
    r.in.snapshot.slot[0].usable = true;
    r.in.crank_play = true;
    r.Step();
    r.in.have_result = true;
    r.in.result = {r.sent.serial, Q::Outcome::Denied, "the game says no", 700, 1, 1};
    r.Step();
    EXPECT_EQ(r.out.crank_answer, -1);
}

TEST(MusicBoxController, Scheme1GripHoldsTriggerDoesNothing) {
    Rig r(1);
    r.Take1(0);
    EXPECT_TRUE(r.out.crank_box);
    r.Step(10);
    // The left trigger's pull: no use, no gun.
    r.Trigger(1.0f);
    r.Step(30);
    r.Trigger(0.0f);
    r.Step(5);
    EXPECT_EQ(r.uses, 0);
    EXPECT_TRUE(r.out.crank_box);
    r.in.crank_play = true;
    r.Step();
    EXPECT_EQ(r.uses, 1);
    EXPECT_EQ(r.sent.crank, 1);
    // Let go of the grip: put away, the weapon back.
    r.in.grip = 0.0f;
    r.Step();
    EXPECT_FALSE(r.out.crank_box);
    EXPECT_TRUE(r.c.GetPhase() == Q::Phase::Idle);
}

TEST(MusicBoxController, ContextAndDeathBringTheWeaponBack) {
    {
        Rig r(2);
        r.Take2(0);
        ASSERT_TRUE(r.out.crank_box);
        r.in.gameplay = false; // a menu, the screen, a cinematic
        r.Step();
        EXPECT_FALSE(r.out.crank_box);
        EXPECT_FALSE(MB::RightHandFree({r.out.crank_box, r.in.gameplay, true}));
    }
    {
        Rig r(2);
        r.Take2(0);
        ASSERT_TRUE(r.out.crank_box);
        r.in.snapshot.alive = false;
        r.Step();
        EXPECT_FALSE(r.out.crank_box);
    }
    {
        Rig r(2);
        r.Take2(0);
        ASSERT_TRUE(r.out.crank_box);
        r.c.Cancel(r.out, "recenter");
        r.Step();
        EXPECT_FALSE(r.out.crank_box);
    }
    {
        // Y (the arc's toggle) puts it away too.
        Rig r(1);
        r.Take1(0);
        ASSERT_TRUE(r.out.crank_box);
        r.in.y = true;
        r.Step();
        EXPECT_FALSE(r.out.crank_box);
    }
}

TEST(MusicBoxController, CrankOffKeepsTheHoldRing) {
    Rig r(2, false);
    r.Take2(0);
    EXPECT_FALSE(r.out.crank_box);
    r.Step(10);
    EXPECT_TRUE(r.ring); // the ring fills, as before
    r.in.crank_play = true;
    r.Step();
    EXPECT_EQ(r.uses, 0);
    // Other items never become crank items.
    Rig other(2, true);
    other.Take2(1);
    EXPECT_FALSE(other.out.crank_box);
    other.Step(5);
    EXPECT_TRUE(other.ring);
}

TEST(MusicBox, ReturningFromTheAxisDoesNotInventATurn) {
    const MB::Config cfg{};
    const MB::Geometry g{};
    const auto f = Box();
    MB::Crank c;
    double t = 10;
    ASSERT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 0)));
    Turn(c, f, t, 0, 0.5f, 0.5);
    const float progress = c.Progress(cfg);
    c.Update(cfg, g, f, HandAt(f, 0, 0.001f), t += Dt);
    c.Update(cfg, g, f, HandAt(f, MB::Pi + 0.5f), t += Dt);
    EXPECT_NEAR(c.Progress(cfg), progress, 1e-5f);
    c.Update(cfg, g, f, HandAt(f, MB::Pi + 0.6f), t += Dt);
    EXPECT_GT(c.Progress(cfg), progress); // observed motion resumes normally
}

TEST(MusicBox, BrokenTrackingCannotGrabOrAdvanceTheCrank) {
    const MB::Config cfg{};
    const MB::Geometry g{};
    const auto f = Box();
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
        MB::Crank c;
        EXPECT_FALSE(c.TryGrab(cfg, g, f, {invalid, 0, 0}));
        ASSERT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 0)));
        const auto step = c.Update(cfg, g, f, {invalid, 0, 0}, 1);
        EXPECT_FALSE(step.play);
        EXPECT_FALSE(c.Grabbed());
        EXPECT_TRUE(std::isfinite(c.Angle()));
        EXPECT_TRUE(std::isfinite(c.Progress(cfg)));
        ASSERT_TRUE(c.TryGrab(cfg, g, f, HandAt(f, 0)));
        c.Update(cfg, g, MB::Frame{}, HandAt(f, 0.5f), 2);
        EXPECT_FALSE(c.Grabbed());
    }
}

} // Anonymous namespace

TEST(MusicBox, NativeCrankMatchesRenderedKnobAtEveryWorldOrientation) {
    namespace B = VR::Body;
    const MB::V3 mid{.0175745f,.01475f,.033567f};
    for(float yaw : {0.0f, .7f, -2.4f}) for(float scale : {.5f,.8f,1.6f}) {
        const auto rotation = B::AxisAngle({0,1,0},yaw);
        const B::Vec3 origin{.3f,1.1f,-.7f};
        auto track = [&](B::Vec3 p) { auto t=B::WorldToTracking(origin+B::Rotate(rotation,p)*scale,.3f,2); return Q::V3{t.x,t.y,t.z}; };
        const auto o=track({});
        const auto f=MB::FrameForModel(o,track({1,0,0})-o,track({0,1,0})-o,track({0,0,1})-o,scale/2,mid);
        MB::Geometry g{}; g.arm_m*=scale/2; g.knob_out_m*=scale/2;
        for(int step=0;step<24;step++) {
            const float a=MB::TwoPi*step/24;
            const auto expected=track({MB::NativeHub.x+std::sin(a)*MB::Geometry{}.arm_m,
                                      MB::NativeHub.y+.5f*MB::Geometry{}.knob_out_m,
                                      MB::NativeHub.z-std::cos(a)*MB::Geometry{}.arm_m});
            EXPECT_LT(Q::Length(MB::KnobAt(g,f,a)-expected),1e-5f);
        }
    }
}

TEST(MusicBox, PinchTargetsPadsInsteadOfWeaponOrigin) {
    namespace B=VR::Body;
    const auto pinch=MB::CrankContact(B::C0000::Test::TestSkeleton());
    ASSERT_TRUE(B::IsFinite(pinch.centre));
    ASSERT_NEAR(B::Dot(pinch.basis,pinch.basis),1,1e-5);
    for(float angle : {0.f,1.2f,3.1f,-2.f}) {
        B::ModelTarget contact{{.3f,-.2f,.7f},B::AxisAngle({0,1,0},angle)};
        B::QsTransform attachment{{-.12f,.04f,.07f},B::AxisAngle({1,0,0},.8f),{1,1,1}};
        const auto target=MB::CrankHandTarget(contact,attachment,pinch);
        const auto wrist=B::Normalize(target.q*B::Conjugate(attachment.q));
        const auto wristPos=target.p-B::Rotate(wrist,attachment.t);
        EXPECT_LT(B::Length(wristPos+B::Rotate(wrist,pinch.centre)-contact.p),1e-5f);
        EXPECT_NEAR(std::abs(B::Dot(wrist*pinch.basis,contact.q*MB::CrankWristRoll)),1,1e-5);
        // Wrist remains behind the pinch, instead of being planted inside the case.
        EXPECT_LT(B::Dot(wristPos-contact.p,B::Rotate(contact.q,{0,0,1})),0);
    }
}

TEST(MusicBox, CrankPinchKeepsThumbAboveIndexAndWristOutsideForFullTurn) {
    namespace B = VR::Body;
    const auto sk = B::C0000::Test::TestSkeleton();
    const auto pinch = MB::CrankContact(sk);
    const auto shape = MB::CrankPinch();
    std::array<B::Vec3, 2> tips{};
    for (int f = 0; f < 2; ++f) {
        B::QsTransform joint{};
        for (int j = 0; j < 3; ++j) {
            const auto& ref = sk.local[B::Grip::RightFingers[f][j]];
            joint = B::Compose(joint, {{ref.t[0], ref.t[1], ref.t[2]},
                B::Grip::ShapeLocal(shape, f, j, 1, 1, sk, B::C0000::Right), {1,1,1}});
        }
        tips[f] = joint.t + B::Rotate(joint.q, {MB::TipLengths[f], 0, 0});
    }
    const auto body = [](MB::V3 v) { return B::Vec3{v.x, v.y, v.z}; };
    for (float yaw : {0.f, .7f, -2.4f}) for (float tilt : {0.f, .8f, -1.1f}) {
        const auto rotation = B::AxisAngle({0,1,0}, yaw) * B::AxisAngle({1,0,0}, tilt);
        const auto track = [&](B::Vec3 p) {
            const auto t = B::WorldToTracking(B::Rotate(rotation, p), .3f, 1);
            return MB::V3{t.x, t.y, t.z};
        };
        const auto frame = MB::FrameForModel({}, track({1,0,0}), track({0,1,0}),
                                             track({0,0,1}), 1, {});
        const auto contactQ = B::TrackingToWorldRotation(
            B::FromBasis(body(frame.u), body(frame.v), body(frame.axis)), .3f);
        const auto up = B::Rotate(rotation, body(MB::NativeUp));
        const auto outward = B::Rotate(rotation, body(MB::NativeAxis));
        const B::QsTransform attachment{{-.12f,.04f,.07f}, B::AxisAngle({1,0,0}, .8f), {1,1,1}};
        B::Quat firstWrist{};
        for (int step = 0; step <= 24; ++step) {
            const auto knob = B::TrackingToWorld(body(MB::KnobAt({}, frame, MB::TwoPi*step/24)), .3f, 1);
            const auto target = MB::CrankHandTarget({knob, contactQ}, attachment, pinch);
            const auto wristQ = B::Normalize(target.q * B::Conjugate(attachment.q));
            const auto wrist = target.p - B::Rotate(wristQ, attachment.t);
            const auto thumb = wrist + B::Rotate(wristQ, tips[0]);
            const auto index = wrist + B::Rotate(wristQ, tips[1]);
            EXPECT_GT(B::Dot(thumb-index, up), .005f);
            EXPECT_GT(B::Dot(wrist-knob, outward), 0.f);
            EXPECT_LT(B::Length((thumb+index)*.5f-knob), 1e-5f);
            if (!step) firstWrist = wristQ;
            EXPECT_NEAR(std::abs(B::Dot(firstWrist, wristQ)), 1.f, 1e-5f);
        }
    }
}

TEST(MusicBox, OriginalCrankRemovalIsAtomicAndPreservesAdjacentCase) {
    struct Vertex { float position[3]; };
    const std::vector<Vertex> vertices{{{.026f,-.014f,-.006f}},{{.029f,-.020f,-.004f}},
                                       {{.03f,-.040f,-.01f}},{{.04f,-.014f,-.006f}}};
    std::vector<unsigned> indices;
    for(int i=0;i<48;i++)indices.insert(indices.end(),{0,1,2});
    indices.insert(indices.end(),{0,1,3}); // shared position bounds must not cut this case triangle
    auto wrong=indices; wrong.erase(wrong.begin(),wrong.begin()+3);
    const auto before=wrong;
    EXPECT_FALSE(MB::RemoveOriginalCrank(vertices,wrong));
    EXPECT_EQ(wrong,before);
    ASSERT_TRUE(MB::RemoveOriginalCrank(vertices,indices));
    EXPECT_EQ(indices,(std::vector<unsigned>{0,1,3}));
    indices={0,1,99};
    EXPECT_FALSE(MB::RemoveOriginalCrank(vertices,indices));
    EXPECT_EQ(indices,(std::vector<unsigned>{0,1,99}));
}

TEST(MusicBox, NativeSideCrankCompletesSlowTurnWithoutRestartingTune) {
    const auto f=MB::FrameForModel({}, {1,0,0},{0,1,0},{0,0,-1},1,{});
    const MB::Config cfg{}; const MB::Geometry g{}; MB::Crank crank;
    ASSERT_TRUE(crank.TryGrab(cfg,g,f,MB::KnobAt(g,f,0)));
    int plays=0; double t=1;
    // 12 seconds per revolution: slow movement must still complete a full tune trigger.
    for(int step=1;step<=1085;step++) {
        t+=Dt;
        plays+=crank.Update(cfg,g,f,MB::KnobAt(g,f,MB::TwoPi*step/1080),t).play;
    }
    ASSERT_EQ(plays,1);
    // Fast additional winding during the native use/cue window must not restart it.
    for(int step=1;step<=180;step++) {
        t+=Dt;
        plays+=crank.Update(cfg,g,f,MB::KnobAt(g,f,MB::TwoPi*(1085.f/1080+step/90.f)),t).play;
    }
    EXPECT_EQ(plays,1);
    crank.Release();
    EXPECT_EQ(crank.Plays(),1); // release is not a cancellation/restart
}

TEST(MusicBox, RegalAtlasDoesNotChangeGeometryOrNormals) {
    struct Vertex { float position[3],normal[3],uv[2]; };
    std::vector<Vertex> vertices{{{.055f,0,0},{1,0,0},{0,0}},{{.055f,.04f,0},{1,0,0},{0,0}},
                                 {{.055f,0,.04f},{1,0,0},{0,0}},{{0,0,.04f},{0,-1,0},{0,0}}};
    std::vector<unsigned> indices{0,1,2,0,2,3};
    const auto before=vertices; const auto order=indices;
    ASSERT_TRUE(MB::ApplyRegalUVs(vertices,indices));
    ASSERT_EQ(vertices.size(),6u);ASSERT_EQ(indices.size(),order.size());
    for(size_t i=0;i<indices.size();i++) {
        EXPECT_EQ(indices[i],i);
        for(int k=0;k<3;k++) {
            EXPECT_EQ(vertices[i].position[k],before[order[i]].position[k]);
            EXPECT_EQ(vertices[i].normal[k],before[order[i]].normal[k]);
        }
        for(float uv:vertices[i].uv) { EXPECT_GE(uv,.006f);EXPECT_LE(uv,.994f); }
    }
    // The lid and side share vertices in the source; the atlas seam must split them.
    EXPECT_LT(vertices[0].uv[0],.5f);EXPECT_GT(vertices[3].uv[0],.5f);
}

TEST(MusicBox, InvalidAtlasMeshIsNotPartlyRemapped) {
    struct Vertex { float position[3],normal[3],uv[2]; };
    std::vector<Vertex> vertices(1);vertices[0].uv[0]=.123f;
    std::vector<unsigned> indices{0,0,5};
    EXPECT_FALSE(MB::ApplyRegalUVs(vertices,indices));
    EXPECT_EQ(vertices.size(),1u);EXPECT_EQ(vertices[0].uv[0],.123f);
    EXPECT_EQ(indices,(std::vector<unsigned>{0,0,5}));
}

TEST(MusicBox, RegalDdsRejectsWrongFormatDimensionsAndTruncation) {
    std::vector<char> data(128+MB::RegalBytes);
    const auto word=[&](size_t at,std::uint32_t v){for(int i=0;i<4;i++)data[at+i]=static_cast<char>(v>>(8*i));};
    word(0,0x20534444);word(4,124);word(12,1024);word(16,1024);word(28,11);word(76,32);word(84,0x31545844);
    ASSERT_TRUE(MB::RegalDdsValid(data));
    word(12,512);EXPECT_FALSE(MB::RegalDdsValid(data));word(12,1024);
    word(84,0x35545844);EXPECT_FALSE(MB::RegalDdsValid(data));word(84,0x31545844);
    data.pop_back();EXPECT_FALSE(MB::RegalDdsValid(data));
}

TEST(MusicBox, SocketIsRestrictedToCrankWallAndInterpolatesAtAxle) {
    // Broad case panels: only the positive-Y wall gets the socket atlas tile.
    const float a[3]{-.012f,.074f,.002f}, b[3]{.05f,.074f,.002f},
                c[3]{.05f,.074f,.083f};
    const auto wall=MB::RegalSurface(a,b,c);
    ASSERT_EQ(wall.tile,2);
    const float leftA[3]{-.012f,-.033f,.002f}, leftB[3]{.05f,-.033f,.002f},
                leftC[3]{.05f,-.033f,.083f};
    EXPECT_EQ(MB::RegalSurface(leftA,leftB,leftC).tile,1);
    const float frontA[3]{-.012f,.074f,.002f}, frontB[3]{.05f,.074f,.002f},
                frontC[3]{.05f,-.033f,.002f};
    EXPECT_EQ(MB::RegalSurface(frontA,frontB,frontC).tile,1);
    const float underA[3]{-.012f,.074f,.002f}, underB[3]{-.012f,-.033f,.002f},
                underC[3]{-.012f,-.033f,.083f};
    EXPECT_EQ(MB::RegalSurface(underA,underB,underC).tile,4);
    // Rasterization interpolates vertex UVs. Clamping each vertex after shifting
    // the atlas would move the painted socket away from this physical axle.
    const float wc=(MB::NativeHub.z-a[2])/(c[2]-a[2]);
    const float wb=(MB::NativeHub.x-a[0])/(b[0]-a[0])-wc;
    const float wa=1-wb-wc;
    ASSERT_GE(wa,0); ASSERT_GE(wb,0); ASSERT_GE(wc,0);
    const auto ua=MB::RegalUV(wall,a), ub=MB::RegalUV(wall,b), uc=MB::RegalUV(wall,c);
    EXPECT_NEAR(wa*ua[0]+wb*ub[0]+wc*uc[0],.249f,2e-5f);
    EXPECT_NEAR(wa*ua[1]+wb*ub[1]+wc*uc[1],.665f,2e-5f);
}
