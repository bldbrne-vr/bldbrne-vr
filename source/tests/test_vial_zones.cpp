#include "core/vr/vial_contact.h"
// SPDX-License-Identifier: GPL-2.0-or-later

// The Blood Vial's stab zones and their gate (src/core/vr/vial_zones.h), played frame by frame at
// 90 Hz with the runtime's velocities: every stab the user asked for injects exactly once (the
// chest, the belly, both sides, the upper thighs, the right forearm, the wrist, the inner elbow,
// the inner upper arm, seated the lap, a taller player), and nothing else ever does (armed or
// resting inside, the weapon arm swinging onto a still vial hand at 3 m/s, the gun's aim pose,
// walking at 1.4 m/s and turning at 90 deg/s with the vial held, a hand lowered to hang at the
// side, slow or glancing entries, one-frame tracking glitches), with the near miss's rule.

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>

#include "c0000_test_skeleton.h"
#include "core/vr/belt_vial.h"
#include "core/vr/body_presets.h"
#include "core/vr/vial_zones.h"

namespace {

namespace Z = VR::VialZones;
namespace B = VR::Body;
using V = B::Vec3;
using Q = B::Quat;

constexpr float Hz = 90.0f;
constexpr float Dt = 1.0f / Hz;
constexpr float Pi = 3.14159265f;

Q Grip(float yaw, float pitch, float roll = 0.0f) {
    const auto a = VR::BodyPresets::GripRotation(yaw, pitch, roll);
    return {a[0], a[1], a[2], a[3]};
}

// The head at (0, 1.6, 0) looking down -Z (the mock's), so the neck pivot is at (0, 1.525, 0.08):
// a point `ahead` of it, `side` to its right (+X) and `h` below it.
V At(float ahead, float side, float h) {
    return {side, 1.525f - h, 0.08f - ahead};
}

// One frame of the world (LOCAL): the eyes and their orientation, both grips.
struct World {
    V eye{0.0f, 1.6f, 0.0f};
    Q head{};
    V left{-0.22f, 1.10f, -0.30f}; // the mock's guard
    Q left_q{};
    V right{0.18f, 1.25f, -0.40f};
    Q right_q = Grip(0.0f, 45.0f);
    bool left_valid{true};
    // A glitch: the reported velocity of the left grip instead of the motion's own.
    bool left_v_override{false};
    V left_v{};
};
using Script = std::function<World(float)>;

double MinJerk(double s) {
    return VR::BodyPresets::MinJerk(s);
}
struct Key {
    float t;
    V p;
};
// Minimum jerk from key to key (held before the first and after the last).
V Path(const std::vector<Key>& keys, float t) {
    if (t <= keys.front().t) {
        return keys.front().p;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const float s = (t - keys[i - 1].t) / std::max(keys[i].t - keys[i - 1].t, 1e-6f);
            return B::Lerp(keys[i - 1].p, keys[i].p, static_cast<float>(MinJerk(s)));
        }
    }
    return keys.back().p;
}

struct SimRun {
    int stabs{0};
    std::vector<std::string> labels;
    std::vector<Z::Entry> hits;
    std::vector<Z::Entry> misses;
    int reaching{0};
    std::array<float, Z::ZoneCount> deepest{}; // the furthest in each zone went (m, > 0 = in)
};

struct Sim {
    Z::Params params{};
    Z::ArmInGrip arm = Z::HunterArmInGrip(0.0f);
    Z::NeckTracker neck{};
    VR::Belt::Torso torso{};
    Z::Gate gate{};
    Z::Geometry geo{};
    float t{0.0f};
    float dt{Dt};

    SimRun Play(const Script& f, float seconds) {
        SimRun run{};
        run.deepest.fill(-10.0f);
        const int n = static_cast<int>(std::lround(seconds / dt));
        for (int i = 0; i < n; ++i, t += dt) {
            const World w = f(t);
            constexpr float h = 1e-3f;
            const World a = f(t - h);
            const World b = f(t + h);
            const auto vel = [&](V World::*m) { return (b.*m - a.*m) * (0.5f / h); };
            const V fwd = B::Rotate(w.head, {0.0f, 0.0f, -1.0f});
            torso.Update({fwd.x, fwd.y, fwd.z}, dt);
            const auto nk = neck.Update(true, w.eye, vel(&World::eye), true, w.head, dt);
            const VR::Belt::P3 tf = torso.Forward();
            const Z::Tracked weapon{true, w.right, vel(&World::right), true, w.right_q};
            geo = Z::Build(nk, {tf.x, tf.y, tf.z}, weapon, arm, params);
            const Z::Tracked vial{w.left_valid, w.left,
                                  w.left_v_override ? w.left_v : vel(&World::left), true,
                                  w.left_q};
            const Z::Result r = gate.Update(geo, vial, dt);
            for (std::size_t z = 0; z < Z::ZoneCount; ++z) {
                if (r.m[z].valid) {
                    run.deepest[z] = std::max(run.deepest[z], -r.m[z].d);
                }
            }
            if (r.stab) {
                ++run.stabs;
                run.labels.emplace_back(r.hit.label);
                run.hits.push_back(r.hit);
            }
            if (r.refused) {
                run.misses.push_back(r.miss);
            }
            run.reaching += r.reaching ? 1 : 0;
        }
        return run;
    }
};

std::string Describe(const SimRun& run) {
    std::string s = "stabs " + std::to_string(run.stabs);
    for (const auto& h : run.hits) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), " | STAB %s %.0f cm in, %.2f m/s (best %.2f), out %.0f cm "
                                        "%.2f s before",
                      h.label, h.inside_m * 100.0f, h.approach, h.best, h.out_m * 100.0f,
                      h.out_age);
        s += buf;
    }
    for (const auto& m : run.misses) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), " | miss %s (%s) %s: %.2f of %.2f m/s, out %.0f cm %.2f s",
                      m.label, Z::ZoneLabel(m.zone), Z::RefusalName(m.why), m.approach, m.speed,
                      m.out_m * 100.0f, m.out_age);
        s += buf;
    }
    return s;
}

bool OneOf(const std::string& label, std::initializer_list<const char*> allowed) {
    for (const char* a : allowed) {
        if (label == a) {
            return true;
        }
    }
    return false;
}

// A stab: held at `from` for 0.5 s (outside), in to `to` over `stroke` s, held there 0.6 s.
SimRun Stab(Sim& sim, V from, V to, float stroke = 0.3f, World base = {}) {
    const std::vector<Key> keys{{0.0f, from}, {0.5f, from}, {0.5f + stroke, to},
                                {1.1f + stroke, to}};
    const float t0 = sim.t;
    return sim.Play(
        [=](float t) {
            World w = base;
            w.left = Path(keys, t - t0);
            return w;
        },
        1.2f + stroke);
}

void ExpectOneStab(const SimRun& run, std::initializer_list<const char*> allowed, const char* what) {
    EXPECT_EQ(run.stabs, 1) << what << ": " << Describe(run);
    if (!run.labels.empty()) {
        EXPECT_TRUE(OneOf(run.labels.front(), allowed)) << what << ": " << Describe(run);
    }
    std::printf("  [%s] %s\n", what, Describe(run).c_str());
}

void ExpectNoStab(const SimRun& run, const char* what) {
    EXPECT_EQ(run.stabs, 0) << what << ": " << Describe(run);
    std::printf("  [%s] %s\n", what, Describe(run).c_str());
}

// ---- The geometry ---------------------------------------------------------------------------------

TEST(VialZones, HunterWristAndElbowOnTheController) {
    // The skeleton's bind pose (the weapon bone on the grip, blade_pitch 0, no trims; the tests'
    // mannequin, tests/c0000_test_skeleton.h): the wrist the weapon bone's offset from the grip
    // point (on the hunter 7.9 cm, toward grip +Y and the pinky's end +Z), the relaxed elbow a
    // forearm behind it, toward grip +Y.
    const auto& sk = B::C0000::Test::TestSkeleton();
    const Z::ArmInGrip a = Z::HunterArmInGrip(0.0f);
    EXPECT_TRUE(a.ok);
    EXPECT_NEAR(B::Length(a.wrist), sk.hand_to_weapon, 1e-4f);
    EXPECT_GT(a.wrist.y, 0.0f);
    EXPECT_NEAR(B::Length(a.elbow - a.wrist), sk.forearm_length, 1e-4f);
    const V f = B::Normalize(a.elbow - a.wrist);
    EXPECT_GT(f.y, 0.5f);
    // A blade pitched 20 degrees toward grip +Y turns the arm 20 degrees about grip +X.
    const Z::ArmInGrip p = Z::HunterArmInGrip(20.0f);
    EXPECT_NEAR(B::Length(p.wrist), B::Length(a.wrist), 1e-4f);
    const V x{1.0f, 0.0f, 0.0f};
    EXPECT_NEAR(p.wrist.x, a.wrist.x, 1e-4f);
    EXPECT_NEAR(B::Angle(B::Perp(p.wrist, x), B::Perp(a.wrist, x)), 20.0f * Pi / 180.0f, 0.01f);
}

TEST(VialZones, NeckPivotStaysWhenTheHeadTurnsOrLooksDown) {
    const V eye{0.0f, 1.6f, 0.0f};
    const V n0 = Z::NeckFromEye(eye, Q{});
    EXPECT_NEAR(n0.y, 1.525f, 1e-5f);
    EXPECT_NEAR(n0.z, 0.08f, 1e-5f);
    // Looking 60 degrees down about the neck: the eyes move, the pivot does not.
    const Q down = B::AxisAngle({1.0f, 0.0f, 0.0f}, -60.0f * Pi / 180.0f);
    const V eye_down = n0 + B::Rotate(down, V{0.0f, Z::NeckUp, -Z::NeckForward});
    const V n1 = Z::NeckFromEye(eye_down, down);
    EXPECT_NEAR(B::Length(n1 - n0), 0.0f, 1e-5f);
    // The tracker: a head turning at 90 deg/s about the neck; the eyes move at 0.1 m/s, the neck
    // is still.
    Z::NeckTracker tracker{};
    float worst = 0.0f;
    for (int i = 0; i < 90; ++i) {
        const float a = 90.0f * Pi / 180.0f * static_cast<float>(i) * Dt;
        const Q q = B::AxisAngle({0.0f, 1.0f, 0.0f}, a);
        const V e = n0 + B::Rotate(q, V{0.0f, Z::NeckUp, -Z::NeckForward});
        const V ev = B::Cross(V{0.0f, 90.0f * Pi / 180.0f, 0.0f}, e - n0);
        const auto out = tracker.Update(true, e, ev, true, q, Dt);
        ASSERT_TRUE(out.ok);
        if (i > 0) {
            worst = std::max(worst, B::Length(out.v));
        }
        EXPECT_NEAR(B::Length(out.p - n0), 0.0f, 1e-4f);
    }
    EXPECT_LT(worst, 0.01f);
}

TEST(VialZones, BodyCoversTheChestBellySidesAndThighsButNotAHangingHand) {
    Sim sim{};
    sim.Play([](float) { return World{}; }, 0.1f);
    const auto in_body = [&](V p) { return Z::MeasureZone(sim.geo, Z::Zone::Body, p).d; };
    // The grip stopped 6 cm out from the skin (the controller in the way) is in.
    EXPECT_LT(in_body(At(0.20f, 0.0f, 0.275f)), -0.02f);  // the chest
    EXPECT_LT(in_body(At(0.20f, 0.0f, 0.475f)), -0.02f);  // the belly
    EXPECT_LT(in_body(At(0.03f, -0.21f, 0.40f)), -0.02f); // the left flank
    EXPECT_LT(in_body(At(0.03f, 0.21f, 0.40f)), -0.02f);  // the right flank
    EXPECT_LT(in_body(At(0.205f, -0.09f, 0.725f)), -0.02f); // the front of the left thigh
    EXPECT_LT(in_body(At(0.205f, 0.09f, 0.725f)), -0.02f);  // the right thigh
    // Out: a hand hanging at the side (its grip 22-26 cm out at the hip), held in front where it
    // waits (30 cm ahead), raised to the face, below the knees.
    for (float side : {0.22f, 0.24f, 0.26f}) {
        for (float h : {0.65f, 0.75f, 0.85f}) {
            EXPECT_GT(in_body(At(0.03f, -side, h)), 0.025f) << side << " " << h;
        }
    }
    EXPECT_GT(in_body(At(0.33f, -0.10f, 0.40f)), 0.05f);
    EXPECT_GT(in_body(At(0.25f, 0.0f, -0.02f)), 0.05f); // in front of the face
    EXPECT_GT(in_body(At(0.10f, 0.0f, 1.10f)), 0.10f);  // the knees
    // The labels.
    EXPECT_STREQ(Z::MeasureZone(sim.geo, Z::Zone::Body, At(0.2f, 0.0f, 0.25f)).label, "chest");
    EXPECT_STREQ(Z::MeasureZone(sim.geo, Z::Zone::Body, At(0.2f, 0.0f, 0.5f)).label, "belly");
    EXPECT_STREQ(Z::MeasureZone(sim.geo, Z::Zone::Body, At(0.2f, 0.05f, 0.8f)).label, "thigh");
    EXPECT_STREQ(Z::MeasureZone(sim.geo, Z::Zone::Body, At(0.0f, -0.2f, 0.4f)).label,
                 "left side");
    EXPECT_STREQ(Z::MeasureZone(sim.geo, Z::Zone::Body, At(0.0f, 0.2f, 0.4f)).label,
                 "right side");
    // No lap standing.
    EXPECT_FALSE(Z::MeasureZone(sim.geo, Z::Zone::Lap, At(0.3f, 0.0f, 0.55f)).valid);
}

TEST(VialZones, RightArmFollowsTheController) {
    Sim sim{};
    sim.Play([](float) { return World{}; }, 0.1f);
    const Z::Geometry& g = sim.geo;
    ASSERT_TRUE(g.arm_ok);
    ASSERT_TRUE(g.body_ok);
    std::printf("  guard: grip (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f) elbow (%.3f %.3f %.3f) "
                "shoulder (%.3f %.3f %.3f)\n",
                g.grip.x, g.grip.y, g.grip.z, g.wrist.x, g.wrist.y, g.wrist.z, g.elbow.x,
                g.elbow.y, g.elbow.z, g.shoulder.x, g.shoulder.y, g.shoulder.z);
    // The wrist the weapon bone's offset behind the grip point (the skeleton's; ~8 cm); the
    // shoulder beside the neck; the bones' lengths.
    EXPECT_NEAR(B::Length(g.wrist - g.grip), B::C0000::Test::TestSkeleton().hand_to_weapon,
                1e-3f);
    EXPECT_GT(g.wrist.z, g.grip.z + 0.04f);
    EXPECT_NEAR(B::Length(g.elbow - g.wrist), Z::ForearmM, 2e-3f);
    EXPECT_NEAR(B::Length(g.elbow - g.shoulder), Z::UpperArmM, 2e-3f);
    // A guard (the hand 40 cm ahead at chest height, blade up 45 degrees): the elbow below the
    // shoulder and the wrist, out to the right, between them front to back.
    EXPECT_LT(g.elbow.y, g.wrist.y);
    EXPECT_LT(g.elbow.y, g.shoulder.y - 0.1f);
    EXPECT_GT(g.elbow.x, 0.12f);
    EXPECT_GT(g.elbow.z, g.wrist.z);
    EXPECT_LT(g.elbow.z, g.shoulder.z);
    // The zones: the forearm's middle 10 cm from its axis is in, 15 cm is not.
    const V mid = B::Lerp(g.wrist, g.elbow, 0.5f);
    EXPECT_LT(Z::MeasureZone(g, Z::Zone::Forearm, mid + V{0.0f, 0.095f, 0.0f}).d, 0.0f);
    EXPECT_GT(Z::MeasureZone(g, Z::Zone::Forearm, mid + V{0.0f, 0.15f, 0.0f}).d, 0.0f);
    EXPECT_STREQ(Z::MeasureZone(g, Z::Zone::Forearm, mid).label, "forearm");
    EXPECT_STREQ(Z::MeasureZone(g, Z::Zone::Forearm, g.elbow).label, "elbow");
    // The old wrist stab of the mock scenarios (lhand 0.36 0.10 -0.04 from the guard) still lands.
    EXPECT_LT(Z::MeasureZone(g, Z::Zone::Wrist, V{0.14f, 1.20f, -0.34f}).d, -0.02f);
    // No right controller: no arm zones (the body stays).
    Z::Tracked none{};
    const Z::Geometry g2 = Z::Build({true, g.neck, {}}, g.forward, none, sim.arm, sim.params);
    EXPECT_FALSE(Z::MeasureZone(g2, Z::Zone::Forearm, mid).valid);
    EXPECT_TRUE(Z::MeasureZone(g2, Z::Zone::Body, mid).valid);
}

// ---- Stabs that inject ------------------------------------------------------------------------------

TEST(VialZones, StabsIntoTheBodyInjectOnce) {
    {
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.45f, -0.05f, 0.30f), At(0.20f, 0.0f, 0.275f)), {"chest"},
                      "chest centre");
    }
    {
        Sim sim{};
        // A short, gentle one: 12 cm in 0.4 s.
        ExpectOneStab(Stab(sim, At(0.32f, -0.02f, 0.29f), At(0.20f, 0.0f, 0.275f), 0.4f),
                      {"chest"}, "chest, a short gentle jab");
    }
    {
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.45f, -0.08f, 0.45f), At(0.20f, 0.0f, 0.475f)), {"belly"},
                      "belly");
    }
    {
        // A slow, deliberate one: 20 cm in 0.6 s.
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.40f, -0.06f, 0.48f), At(0.20f, 0.0f, 0.50f), 0.6f), {"belly"},
                      "belly, slow and deliberate");
    }
    {
        // A short tap from just outside: 7 cm in 0.2 s.
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.28f, 0.0f, 0.45f), At(0.21f, 0.0f, 0.45f), 0.2f), {"belly"},
                      "belly, a 7 cm tap");
    }
    {
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.05f, -0.45f, 0.40f), At(0.03f, -0.21f, 0.40f)),
                      {"left side"}, "left side");
    }
    {
        Sim sim{};
        // The right flank, the left hand reaching across: the right elbow is right there.
        ExpectOneStab(Stab(sim, At(0.10f, 0.45f, 0.42f), At(0.03f, 0.21f, 0.42f)),
                      {"right side", "elbow", "upper arm", "forearm"}, "right side");
    }
    {
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.45f, -0.12f, 0.62f), At(0.205f, -0.09f, 0.725f)), {"thigh"},
                      "left upper thigh");
    }
    {
        Sim sim{};
        ExpectOneStab(Stab(sim, At(0.45f, 0.05f, 0.62f), At(0.205f, 0.09f, 0.725f)), {"thigh"},
                      "right upper thigh");
    }
}

TEST(VialZones, StabsIntoTheRightArmInjectOnce) {
    Sim probe{};
    probe.Play([](float) { return World{}; }, 0.1f);
    const Z::Geometry g = probe.geo;
    const V up{0.0f, 1.0f, 0.0f};
    const V left{-1.0f, 0.0f, 0.0f};
    {
        // The forearm's middle, from above: the grip stops 9.5 cm over its axis.
        Sim sim{};
        const V mid = B::Lerp(g.wrist, g.elbow, 0.5f);
        ExpectOneStab(Stab(sim, mid + up * 0.35f + left * 0.10f, mid + up * 0.095f), {"forearm"},
                      "right forearm, middle");
    }
    {
        // The wrist, from the left (the inside).
        Sim sim{};
        ExpectOneStab(Stab(sim, g.wrist + left * 0.35f + up * 0.05f, g.wrist + left * 0.10f),
                      {"wrist", "forearm"}, "right wrist");
    }
    {
        // The old mock stab (lhand 0.35 0.36 0.10 -0.04 from the guard's left grip).
        Sim sim{};
        ExpectOneStab(Stab(sim, V{-0.22f, 1.10f, -0.30f}, V{0.14f, 1.20f, -0.34f}, 0.35f),
                      {"wrist"}, "the mock's wrist stab");
    }
    {
        // The inside of the elbow (toward the body's middle and a little ahead).
        Sim sim{};
        const V inner = g.elbow + left * 0.09f + V{0.0f, 0.0f, -0.02f};
        ExpectOneStab(Stab(sim, inner + left * 0.25f + V{0.0f, 0.05f, -0.15f}, inner),
                      {"elbow", "forearm", "upper arm", "right side", "chest", "belly"},
                      "inner elbow");
    }
    {
        // The inner upper arm above the elbow (where the user found it worked).
        Sim sim{};
        const V upper = B::Lerp(g.elbow, g.shoulder, 0.4f);
        const V inner = upper + left * 0.10f;
        ExpectOneStab(Stab(sim, inner + left * 0.25f + V{0.0f, 0.0f, -0.2f}, inner),
                      {"upper arm", "elbow", "chest", "right side"}, "inner upper arm");
    }
}

TEST(VialZones, OtherArmPoses) {
    // The weapon hand hanging at the side (the controller pointing forward), and held out
    // pointing ahead: its forearm where the arm is, a stab into it from above.
    for (const auto& [grip, q, what] :
         {std::tuple{V{0.24f, 0.86f, -0.02f}, Grip(0.0f, 0.0f), "the arm hanging at the side"},
          std::tuple{V{0.20f, 1.30f, -0.55f}, Grip(0.0f, 10.0f), "the arm held out ahead"}}) {
        Sim probe{};
        probe.Play(
            [&](float) {
                World w{};
                w.right = grip;
                w.right_q = q;
                return w;
            },
            0.1f);
        const Z::Geometry& g = probe.geo;
        std::printf("  %s: grip (%.3f %.3f %.3f) wrist (%.3f %.3f %.3f) elbow (%.3f %.3f %.3f)\n",
                    what, g.grip.x, g.grip.y, g.grip.z, g.wrist.x, g.wrist.y, g.wrist.z, g.elbow.x,
                    g.elbow.y, g.elbow.z);
        // The elbow between the shoulder and the hand, never above the shoulder.
        EXPECT_LT(g.elbow.y, g.shoulder.y);
        EXPECT_GT(g.elbow.y, std::min(g.wrist.y, g.shoulder.y) - 0.15f);
        Sim sim{};
        const V mid = B::Lerp(g.wrist, g.elbow, 0.5f);
        World base{};
        base.right = grip;
        base.right_q = q;
        const V from = mid + V{-0.20f, 0.25f, -0.15f};
        const V to = mid + V{-0.06f, 0.075f, 0.0f};
        ExpectOneStab(Stab(sim, from, to, 0.35f, base), {"forearm", "wrist", "elbow"}, what);
    }
}

TEST(VialZones, AFoldedArmsElbowFollowsTheNaturalArms) {
    // 2026-10-08 review: body_natural_arms (the default) hangs the elbow of an arm folded across
    // the chest lower; the zones kept the old pole. The right hand folded across to the left of
    // the chest, 20 cm in front of the neck, the blade up 60 degrees.
    const V folded{-0.15f, 1.30f, -0.12f};
    const auto geo = [&](bool natural) {
        Sim probe{};
        probe.params.natural_arms = natural;
        probe.Play(
            [&](float) {
                World w{};
                w.right = folded;
                w.right_q = Grip(0.0f, 60.0f);
                return w;
            },
            0.1f);
        return probe.geo;
    };
    const Z::Geometry g = geo(true);
    const Z::Geometry old = geo(false);
    std::printf("  folded: elbow natural (%.3f %.3f %.3f), old (%.3f %.3f %.3f): %.1f cm apart\n",
                g.elbow.x, g.elbow.y, g.elbow.z, old.elbow.x, old.elbow.y, old.elbow.z,
                100.0f * B::Length(g.elbow - old.elbow));
    EXPECT_TRUE(Z::Params{}.natural_arms);
    EXPECT_NEAR(B::Length(g.elbow - g.wrist), Z::ForearmM, 2e-3f);
    EXPECT_NEAR(B::Length(g.elbow - g.shoulder), Z::UpperArmM, 2e-3f);
    EXPECT_GT(B::Length(g.elbow - old.elbow), 0.10f); // 16.9 cm
    EXPECT_LT(g.elbow.y, old.elbow.y - 0.05f);         // hangs 8.4 cm lower
    // A jab up into the drawn elbow's underside (4 cm below it) is in an arm zone with natural on.
    const V under = g.elbow + V{0.0f, -0.04f, 0.0f};
    const float d_natural = std::min(Z::MeasureZone(g, Z::Zone::Forearm, under).d,
                                     Z::MeasureZone(g, Z::Zone::UpperArm, under).d);
    EXPECT_LT(d_natural, -0.02f);
}

TEST(VialZones, SeatedHandsOnTheLap) {
    // Seated, the vial hand resting on the lap: nothing; lifted 15 cm and stabbed back: one.
    Sim sim{};
    sim.params.seated = true;
    const V rest = At(0.30f, -0.10f, 0.62f - 0.075f - 0.12f);
    const V up = rest + V{0.0f, 0.15f, -0.05f};
    const std::vector<Key> keys{{0.0f, rest}, {2.0f, rest}, {2.4f, up}, {2.6f, up}, {2.9f, rest},
                                {3.6f, rest}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = Path(keys, t);
            return w;
        },
        3.7f);
    ExpectOneStab(run, {"lap"}, "seated: resting on the lap, then lifted and stabbed back");
    EXPECT_GT(run.hits.empty() ? 0.0f : sim.t - 2.9f, 0.0f);
}

TEST(VialZones, SeatedLapAndATallerPlayer) {
    {
        Sim sim{};
        sim.params.seated = true;
        sim.params.lap_below = Z::LapBelow;
        // The lap 0.62 below the eyes: its top ~8 cm higher; the grip stops 6 cm over it.
        const V to = At(0.30f, -0.05f, 0.62f - 0.075f - 0.14f);
        ExpectOneStab(Stab(sim, to + V{-0.05f, 0.30f, -0.10f}, to), {"lap", "belly", "thigh"},
                      "seated lap");
    }
    {
        // 190 cm (k 1.14): the chest is lower and further, the zone follows.
        Sim sim{};
        sim.params.k = 1.14f;
        const float k = sim.params.k;
        ExpectOneStab(Stab(sim, At(0.50f * k, -0.05f, 0.30f * k), At(0.20f * k, 0.0f, 0.275f * k)),
                      {"chest"}, "chest, 190 cm");
    }
}

TEST(VialZones, ZoneScaleGrowsTheReach) {
    Sim sim{};
    sim.Play([](float) { return World{}; }, 0.1f);
    const V p = At(0.26f, 0.0f, 0.30f); // 26 cm ahead of the neck at the chest: just out
    const float d1 = Z::MeasureZone(sim.geo, Z::Zone::Body, p).d;
    Z::Geometry g = sim.geo;
    g.params.scale = 2.0f;
    const float d2 = Z::MeasureZone(g, Z::Zone::Body, p).d;
    EXPECT_GT(d1, 0.0f);
    EXPECT_LT(d2, 0.0f);
}

// ---- Nothing else ---------------------------------------------------------------------------------

TEST(VialZones, ArmedWithTheHandAtTheChestHeldStillNeverFires) {
    Sim probe{};
    probe.Play([](float) { return World{}; }, 0.1f);
    const V at = At(0.18f, -0.03f, 0.30f);
    ASSERT_LT(Z::MeasureZone(probe.geo, Z::Zone::Body, at).d, 0.0f);
    Sim sim{};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            // A still hand's tremor: +-1.5 mm at 7 Hz.
            w.left = at + V{0.0015f * std::sin(44.0f * t), 0.0015f * std::cos(31.0f * t), 0.0f};
            return w;
        },
        3.0f);
    ExpectNoStab(run, "armed at the chest, held still");
}

TEST(VialZones, AHandRestingOnTheBellyNeverFires) {
    Sim sim{};
    const V at = At(0.19f, -0.04f, 0.50f);
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            // Breathing (1 cm at 0.25 Hz along the forward) and a slow drift (2 cm over 4 s).
            w.left = at + V{0.005f * t, 0.0f, -0.01f * std::sin(2.0f * Pi * 0.25f * t)};
            return w;
        },
        4.0f);
    ExpectNoStab(run, "resting on the belly");
}

TEST(VialZones, TheWeaponArmSwingingOntoAStillVialHandNeverFires) {
    Sim sim{};
    const V vial{0.05f, 1.20f, -0.35f};
    // The right hand from up and out to the right across to the low left at a 3 m/s peak (min
    // jerk: 1.875 x the average), the blade turning with it, and back.
    const V a{0.55f, 1.45f, -0.25f};
    const V b{-0.35f, 1.05f, -0.45f};
    const float T = 1.875f * B::Length(b - a) / 3.0f;
    const std::vector<Key> keys{{0.0f, a}, {0.4f, a}, {0.4f + T, b}, {1.0f + T, b},
                                {1.0f + 2.0f * T, a}, {1.4f + 2.0f * T, a}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = vial;
            w.right = Path(keys, t);
            const float s = std::clamp((w.right.x - a.x) / (b.x - a.x), 0.0f, 1.0f);
            w.right_q = Grip(-30.0f + 70.0f * s, 45.0f - 45.0f * s, 20.0f * s);
            return w;
        },
        1.4f + 2.0f * T);
    ExpectNoStab(run, "the weapon arm swinging over the still vial hand at 3 m/s");
    // It did go over the vial hand (the near misses say the vial hand was still), and the swings
    // were never held back for it.
    bool still = false;
    for (const auto& m : run.misses) {
        still = still || (Z::IsArm(m.zone) && m.why == Z::Refusal::Still);
    }
    EXPECT_TRUE(still) << Describe(run);
    EXPECT_EQ(run.reaching, 0);
}

TEST(VialZones, TheGunAimPoseNeverFires) {
    Sim sim{};
    // The mock's laim: the hand 45 cm out from the left shoulder (-0.17, 1.38, 0.05) along the
    // aim (straight ahead, 20 degrees right, 30 degrees left), raised from the guard's rest and
    // lowered back, three times each.
    const V rest{-0.22f, 1.10f, -0.30f};
    std::vector<Key> keys{{0.0f, rest}};
    float t = 0.3f;
    for (int i = 0; i < 3; ++i) {
        for (float yaw : {0.0f, -20.0f, 30.0f}) {
            const float y = yaw * Pi / 180.0f;
            const V aim = V{-0.17f, 1.38f, 0.05f} + V{-std::sin(y), 0.0f, -std::cos(y)} * 0.45f;
            keys.push_back({t + 0.35f, aim});
            keys.push_back({t + 1.0f, aim});
            keys.push_back({t + 1.4f, rest});
            t += 1.6f;
        }
    }
    const SimRun run = sim.Play(
        [=](float s) {
            World w{};
            w.left = Path(keys, s);
            return w;
        },
        t + 0.2f);
    ExpectNoStab(run, "the gun aimed and lowered");
}

TEST(VialZones, WalkingAndTurningWithTheVialHeldNeverFire) {
    {
        // Walking forward at 1.4 m/s for 4 s (in the room): the head bobs 2.5 cm at 1.8 Hz, the
        // hands half that a little later and sway 1.5 cm.
        Sim sim{};
        const SimRun run = sim.Play(
            [](float t) {
                World w{};
                const V walk{0.0f, 0.0f, -1.4f * t};
                const float bob = 0.025f * std::sin(2.0f * Pi * 1.8f * t);
                const float lag = 0.0125f * std::sin(2.0f * Pi * 1.8f * t - 0.7f);
                const float sway = 0.015f * std::sin(2.0f * Pi * 0.9f * t);
                w.eye = V{0.0f, 1.6f + bob, 0.0f} + walk;
                w.left = V{-0.15f + sway, 1.20f + lag, -0.35f} + walk;
                w.right = V{0.18f - sway, 1.25f + lag, -0.40f} + walk;
                return w;
            },
            4.0f);
        ExpectNoStab(run, "walking 1.4 m/s, the vial held in front");
    }
    for (const float ahead : {0.42f, 0.30f}) {
        // Turning the whole body at 90 deg/s, a full turn: the head, the guard and the vial held
        // in front (30 cm ahead of the neck: 6 cm outside the chest's zone) turn about the neck.
        Sim sim{};
        const V neck{0.0f, 1.525f, 0.08f};
        const SimRun run = sim.Play(
            [=](float t) {
                const float a = 0.5f * Pi * std::max(t - 0.2f, 0.0f);
                const Q q = B::AxisAngle({0.0f, 1.0f, 0.0f}, a);
                World w{};
                w.head = q;
                w.eye = neck + B::Rotate(q, V{0.0f, Z::NeckUp, -Z::NeckForward});
                w.left = neck + B::Rotate(q, At(ahead, -0.08f, 0.32f) - neck);
                w.right = neck + B::Rotate(q, V{0.18f, 1.25f, -0.40f} - neck);
                w.right_q = q * Grip(0.0f, 45.0f);
                return w;
            },
            4.4f);
        ExpectNoStab(run, ahead > 0.4f ? "turning 90 deg/s, the vial 42 cm ahead"
                                       : "turning 90 deg/s, the vial 30 cm ahead");
    }
}

TEST(VialZones, AHandLoweredToHangAtTheSideNeverFires) {
    Sim sim{};
    const std::vector<Key> keys{{0.0f, V{-0.15f, 1.20f, -0.35f}},
                                {0.3f, V{-0.15f, 1.20f, -0.35f}},
                                {1.0f, V{-0.24f, 0.78f, 0.06f}},
                                {2.0f, V{-0.24f, 0.78f, 0.06f}},
                                {2.6f, V{-0.15f, 1.20f, -0.35f}},
                                {3.0f, V{-0.15f, 1.20f, -0.35f}},
                                {3.5f, V{-0.25f, 0.82f, 0.04f}}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = Path(keys, t);
            return w;
        },
        4.0f);
    ExpectNoStab(run, "lowered to hang at the side, twice");
}

TEST(VialZones, SlowAndGlancingEntriesAreNearMisses) {
    {
        // 15 cm into the belly at 0.15 m/s.
        Sim sim{};
        const V from = At(0.40f, -0.02f, 0.50f);
        const V to = At(0.20f, -0.02f, 0.50f);
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                const float s = std::clamp((t - 0.5f) * 0.15f / 0.20f, 0.0f, 1.0f);
                w.left = B::Lerp(from, to, s);
                return w;
            },
            2.5f);
        ExpectNoStab(run, "15 cm into the belly at 0.15 m/s");
        ASSERT_FALSE(run.misses.empty());
        EXPECT_EQ(run.misses.front().why, Z::Refusal::Slow) << Describe(run);
    }
    {
        // Sweeping sideways across the front of the belly at 1.2 m/s, drifting 8 cm closer over
        // the 60 cm: it slides into the zone's front almost along it.
        Sim sim{};
        const V from = At(0.28f, -0.30f, 0.50f);
        const V to = At(0.20f, 0.30f, 0.50f);
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                const float s = std::clamp((t - 0.3f) * 2.0f, 0.0f, 1.0f);
                w.left = B::Lerp(from, to, s);
                return w;
            },
            1.5f);
        ExpectNoStab(run, "sliding across the belly's front");
        bool glancing = false;
        for (const auto& m : run.misses) {
            glancing = glancing || m.why == Z::Refusal::Glancing;
        }
        EXPECT_TRUE(glancing) << Describe(run);
    }
}

TEST(VialZones, OneFrameTrackingGlitchesNeverFire) {
    {
        // Held in front; one frame the grip jumps into the chest, reported moving at 3 m/s.
        Sim sim{};
        const V hold = At(0.48f, -0.05f, 0.30f);
        const V jump = At(0.15f, 0.0f, 0.30f);
        const int glitch = 60;
        int frame = -1;
        const SimRun run = sim.Play(
            [&](float t) {
                World w{};
                const int f = static_cast<int>(std::lround(t * Hz));
                w.left = f == glitch ? jump : hold;
                if (f == glitch) {
                    w.left_v_override = true;
                    w.left_v = B::Normalize(jump - hold) * 3.0f;
                }
                frame = f;
                return w;
            },
            1.5f);
        (void)frame;
        ExpectNoStab(run, "one frame jumped into the chest");
    }
    {
        // Resting on the belly; one frame the grip jumps 30 cm out (reported at 5 m/s) and back.
        Sim sim{};
        const V rest = At(0.19f, -0.04f, 0.50f);
        const V out = rest + V{0.0f, 0.0f, -0.30f};
        const SimRun run = sim.Play(
            [&](float t) {
                World w{};
                const int f = static_cast<int>(std::lround(t * Hz));
                w.left = f == 60 ? out : rest;
                if (f == 60 || f == 61) {
                    w.left_v_override = true;
                    w.left_v = f == 60 ? V{0.0f, 0.0f, -5.0f} : V{0.0f, 0.0f, 5.0f};
                }
                return w;
            },
            1.5f);
        ExpectNoStab(run, "one frame jumped out of the belly and back");
    }
}

TEST(VialZones, RefusedThenOutAndBackInFires) {
    Sim sim{};
    // Armed inside the chest's zone: nothing; out 15 cm and a stab back in: one.
    const V in = At(0.19f, -0.03f, 0.30f);
    const V out = At(0.40f, -0.06f, 0.30f);
    const std::vector<Key> keys{{0.0f, in}, {1.0f, in}, {1.5f, out}, {1.7f, out}, {2.0f, in},
                                {2.8f, in}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = Path(keys, t);
            return w;
        },
        3.0f);
    ExpectOneStab(run, {"chest"}, "out of the chest and back in");
    EXPECT_GT(run.hits.front().out_m, 0.03f);
    EXPECT_LE(run.hits.front().out_age, Z::RecentOutS);
}

TEST(VialZones, TakenInsideCountsOnceMovedAndNextStabsNeedANewEntry) {
    // The belt's draw at the hip's edge, then straight into the left flank: one stab after the
    // hand has moved MoveOutM; staying there, nothing more; out and back in: a second (the quick
    // items' bloods use the gate again after a stab).
    Sim sim{};
    const V draw = At(0.0f, -0.235f, 0.47f);
    const V flank = At(0.02f, -0.14f, 0.45f);
    sim.Play(
        [=](float) {
            World w{};
            w.left = draw;
            return w;
        },
        0.1f);
    sim.gate.Reset();
    sim.gate.StartMoved(draw);
    const float t0 = sim.t;
    const V away = At(0.05f, -0.45f, 0.45f);
    const std::vector<Key> keys{{0.0f, draw}, {0.3f, draw}, {0.6f, flank}, {1.6f, flank},
                                {2.0f, away}, {2.2f, away}, {2.5f, flank}, {3.2f, flank}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = Path(keys, t - t0);
            return w;
        },
        3.3f);
    EXPECT_EQ(run.stabs, 2) << Describe(run);
    std::printf("  [drawn at the hip, into the flank, out and in again] %s\n",
                Describe(run).c_str());
}

TEST(VialZones, ReachingHoldsSwingsOnlyForTheVialHandsOwnMotion) {
    Sim probe{};
    probe.Play([](float) { return World{}; }, 0.1f);
    const V wrist = probe.geo.wrist;
    Sim sim{};
    // The vial hand moving toward the wrist (at ~0.6 m/s), stopping 12 cm short: swings wait
    // while it comes within 15 cm.
    const V from = wrist + V{-0.55f, 0.05f, 0.0f};
    const V to = wrist + V{-0.26f, 0.0f, 0.0f};
    const SimRun run = Stab(sim, from, to, 0.5f);
    EXPECT_EQ(run.stabs, 0) << Describe(run);
    EXPECT_GT(run.reaching, 0);
}

// ---- The merge stage's skeptic: natural stabs from close by, combat motions with the vial armed ---

// Held just outside a zone for 2 s (the vial ready a hand's width from the chest or the arm), then a
// slow, deliberate stab: it injects (it came from outside and went StrokeM in, by its own motion).
TEST(VialZones, SkepticSlowStabsFromCloseByInject) {
    Sim probe{};
    probe.Play([](float) { return World{}; }, 0.1f);
    const Z::Geometry g = probe.geo;
    const V left{-1.0f, 0.0f, 0.0f};
    const auto rest_then_stab = [](Sim& sim, V from, V to, float speed) {
        const float stroke = B::Length(to - from) / speed;
        const std::vector<Key> keys{{0.0f, from}, {2.0f, from}, {2.0f + stroke, to},
                                    {3.0f + stroke, to}};
        return sim.Play(
            [=](float t) {
                World w{};
                w.left = Path(keys, t);
                return w;
            },
            3.0f + stroke);
    };
    constexpr float ChestFront = 0.24f; // the chest's zone ahead of the neck (h 0.275, k 1)
    for (const float out : {0.02f, 0.01f}) {
        Sim sim{};
        ExpectOneStab(rest_then_stab(sim, At(ChestFront + out, -0.03f, 0.275f),
                                     At(ChestFront - 0.05f, 0.0f, 0.275f), 0.25f),
                      {"chest"},
                      out > 0.015f ? "rested 2 cm out of the chest's zone, 7 cm in at 0.25 m/s"
                                   : "rested 1 cm out of the chest's zone, 6 cm in at 0.25 m/s");
    }
    {
        Sim sim{};
        ExpectOneStab(rest_then_stab(sim, At(ChestFront + 0.10f, -0.03f, 0.30f),
                                     At(ChestFront - 0.04f, 0.0f, 0.30f), 0.25f),
                      {"chest"}, "10 cm out of the chest's zone, 14 cm in at 0.25 m/s");
    }
    {
        const V mid = B::Lerp(g.wrist, g.elbow, 0.5f);
        const float r = Z::ForearmSkin + Z::ArmReach;
        Sim sim{};
        ExpectOneStab(rest_then_stab(sim, mid + left * (r + 0.10f), mid + left * (r - 0.04f), 0.25f),
                      {"forearm", "elbow"}, "10 cm out of the forearm's zone at 0.25 m/s");
        Sim near{};
        ExpectOneStab(rest_then_stab(near, mid + left * (r + 0.02f), mid + left * (r - 0.05f), 0.25f),
                      {"forearm", "elbow"}, "rested 2 cm out of the forearm's zone, 7 cm in");
    }
    {
        const V mid = B::Lerp(g.grip, g.wrist, 0.5f);
        const float r = Z::HandSkin + Z::ArmReach;
        Sim sim{};
        ExpectOneStab(rest_then_stab(sim, mid + left * (r + 0.10f), mid + left * (r - 0.04f), 0.25f),
                      {"wrist", "forearm"}, "10 cm out of the wrist's zone at 0.25 m/s");
    }
}

// The weapon arm swinging with the vial armed: the vial hand pulled back to the chest as the right
// arm swings across (a boxer's other hand), or both hands swinging together (a two-handed swing,
// the left hand closing on the right as they go): never an injection, and swings never wait.
TEST(VialZones, SkepticSwingsWithTheVialArmedNeverFire) {
    const V a{0.45f, 1.45f, -0.25f};
    const V b{-0.30f, 1.05f, -0.50f};
    const float T = 1.875f * B::Length(b - a) / 3.0f;
    {
        Sim sim{};
        const std::vector<Key> right{{0.0f, a}, {0.5f, a}, {0.5f + T, b}, {1.3f + T, b}};
        const V near = At(0.30f, -0.10f, 0.35f); // 6 cm out of the chest's zone
        const V pulled = At(0.18f, -0.12f, 0.37f);
        const std::vector<Key> left{{0.0f, near}, {0.5f, near}, {0.5f + T, pulled},
                                    {1.3f + T, pulled}};
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                w.right = Path(right, t);
                w.left = Path(left, t);
                return w;
            },
            1.3f + T);
        ExpectNoStab(run, "the vial hand pulled back to the chest during a 3 m/s swing");
    }
    {
        // Backhand (left to right) with the left hand trailing the right, 30 cm to 10 cm apart.
        Sim sim{};
        const std::vector<Key> right{{0.0f, b}, {0.5f, b}, {0.5f + T, a}, {1.3f + T, a}};
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                w.right = Path(right, t);
                const float s = std::clamp((t - 0.5f) / T, 0.0f, 1.0f);
                const float gap = 0.30f - 0.20f * static_cast<float>(MinJerk(s));
                w.left = w.right + V{-gap, -0.03f, 0.02f};
                w.right_q = Grip(40.0f - 70.0f * s, 20.0f + 25.0f * s);
                return w;
            },
            1.3f + T);
        ExpectNoStab(run, "both hands swinging together at 3 m/s, closing from 30 to 10 cm");
        EXPECT_EQ(run.reaching, 0) << Describe(run);
    }
}

// A sideways dodge (lean_dodge) with the vial hand hanging at the side: the head 25 cm to the left
// in 0.3 s and back, the hand following the shoulder 70 % (its inertia): the body came to the hand.
// A forward lunge with the vial held near the chest, the hand following 80 %: the same.
TEST(VialZones, SkepticDodgesAndLungesWithTheVialArmedNeverFire) {
    for (const float dir : {-1.0f, 1.0f}) {
        Sim sim{};
        const V hang = At(0.02f, -0.23f, 0.60f);
        const std::vector<Key> lean{{0.0f, V{}}, {0.5f, V{}}, {0.8f, V{0.25f * dir, -0.05f, 0.0f}},
                                    {1.4f, V{0.25f * dir, -0.05f, 0.0f}}, {1.9f, V{}},
                                    {2.4f, V{}}};
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                const V d = Path(lean, t);
                w.eye = w.eye + d;
                w.left = hang + d * 0.7f;
                w.right = w.right + d * 0.7f;
                return w;
            },
            2.4f);
        ExpectNoStab(run, dir < 0.0f ? "a dodge to the left, the vial hand hanging at the left"
                                     : "a dodge to the right, the vial hand hanging at the left");
    }
    {
        Sim sim{};
        const V held = At(0.29f, -0.08f, 0.33f); // 5 cm out of the chest's zone
        const std::vector<Key> lunge{{0.0f, V{}}, {0.5f, V{}}, {0.8f, V{0.0f, -0.12f, -0.30f}},
                                     {1.3f, V{0.0f, -0.12f, -0.30f}}, {1.8f, V{}}, {2.2f, V{}}};
        const SimRun run = sim.Play(
            [=](float t) {
                World w{};
                const V d = Path(lunge, t);
                w.eye = w.eye + d;
                w.left = held + d * 0.8f;
                w.right = w.right + d * 0.8f;
                return w;
            },
            2.2f);
        ExpectNoStab(run, "a forward lunge, the vial held 5 cm out of the chest's zone");
    }
}

// The vial hand throwing (the mock's overhand: back beside the head, forward over the shoulder at
// 5 m/s, a follow-through, back to the guard) and aiming the gun with a recoil kick: never.
TEST(VialZones, SkepticThrowsAndGunKicksNeverFire) {
    Sim sim{};
    const V rest{-0.22f, 1.10f, -0.30f};
    const V over{-0.01f, 1.63f, -0.15f}; // up in front of the face (the mock's V), then back
    const V windup{-0.11f, 1.58f, 0.15f};
    const V release{-0.17f, 1.52f, -0.43f};
    const V follow{-0.15f, 1.30f, -0.58f};
    const V aim{-0.17f, 1.38f, -0.40f};
    const V kick = aim + V{0.0f, 0.04f, 0.03f};
    const std::vector<Key> keys{{0.0f, rest},     {0.3f, rest},     {0.5f, over},   {0.7f, windup},
                                {0.9f, windup},   {1.02f, release}, {1.22f, follow},
                                {1.8f, rest},     {2.2f, aim},      {2.6f, aim},
                                {2.65f, kick},    {2.8f, aim},      {3.0f, aim},
                                {3.05f, kick},    {3.2f, aim},      {3.6f, rest},
                                {4.0f, rest}};
    const SimRun run = sim.Play(
        [=](float t) {
            World w{};
            w.left = Path(keys, t);
            return w;
        },
        4.0f);
    ExpectNoStab(run, "an overhand throw, then the gun aimed with two recoil kicks");
}


TEST(VialZones, HeadStabsFromFrontSideAndAboveInjectOnce) {
    for (const auto& points : {
             std::pair{V{0.0f, 1.65f, -0.28f}, V{0.0f, 1.65f, -0.02f}},
             std::pair{V{-0.36f, 1.65f, 0.08f}, V{-0.09f, 1.65f, 0.08f}},
             std::pair{V{0.0f, 2.02f, 0.08f}, V{0.0f, 1.78f, 0.08f}}}) {
        Sim sim{};
        ExpectOneStab(Stab(sim, points.first, points.second), {"head"}, "head injection");
    }
}

TEST(VialZones, BothKneesAndShinsInjectOnceStandingAndSeated) {
    for (bool seated : {false, true}) {
        for (float side : {-0.12f, 0.12f}) {
            for (float height : {0.50f, 0.30f}) {
                Sim sim{};
                sim.params.floor_known = true;
                sim.params.seated = seated;
                World w{};
                w.eye.y = seated ? 1.15f : 1.60f;
                const float z = seated ? -0.42f : 0.03f;
                ExpectOneStab(Stab(sim, {side, height, z - 0.30f},
                                   {side, height, z - 0.07f}, 0.3f, w),
                              {side < 0 ? "left leg" : "right leg", seated ? "lap" : ""}, "leg injection");
            }
        }
    }
}

TEST(VialZones, BendingDownKeepsShinsAtTheRoomFloor) {
    Sim sim{};
    sim.params.floor_known = true;
    sim.Play([](float) { return World{}; }, 0.1f);
    const V ankle = sim.geo.ankles[0];
    sim.Play([](float) { World w{}; w.eye.y -= 0.55f; return w; }, 0.1f);
    EXPECT_NEAR(B::Length(sim.geo.ankles[0] - ankle), 0.0f, 1e-5f);
    World bent{};
    bent.eye.y -= 0.55f;
    ExpectOneStab(Stab(sim, {-0.12f, 0.30f, -0.27f}, {-0.12f, 0.30f, -0.04f},
                       0.3f, bent), {"left leg", "thigh"}, "bent down to shin");
}

TEST(VialZones, NewTargetsRespectBodyScaleAndFloorOrigin) {
    for (float k : {0.80f, 1.0f, 1.20f}) {
        Sim sim{};
        sim.params.k = k;
        sim.params.floor_known = true;
        sim.params.floor_y = -0.60f;
        World w{};
        w.eye.y = -0.60f + 1.60f * k;
        sim.Play([=](float) { return w; }, 0.1f);
        EXPECT_NEAR(sim.geo.ankles[0].y, -0.60f + 0.12f * k, 1e-5f);
        EXPECT_NEAR(sim.geo.knees[1].y, -0.60f + 0.50f * k, 1e-5f);
        EXPECT_GT(Z::MeasureZone(sim.geo, Z::Zone::LeftLeg,
                                {-0.12f * k, -0.70f, 0.03f}).d, 0.0f);
        ExpectOneStab(Stab(sim, {0.0f, w.eye.y, -0.30f},
                           {0.0f, w.eye.y, -0.01f}, 0.3f, w), {"head"}, "scaled head");
    }
}

TEST(VialZones, ArmingOrRegainingTrackingInsideNewTargetsDoesNotInject) {
    for (V point : {V{-0.09f, 1.65f, 0.08f}, V{-0.12f, 0.3f, -0.04f},
                    V{0.12f, 0.3f, -0.04f}}) {
        Sim sim{};
        sim.params.floor_known = true;
        ExpectNoStab(sim.Play([=](float t) {
            World w{}; w.left = point; w.left_valid = t < 0.4f || t > 0.6f; return w;
        }, 1.2f), "resting at new target across tracking gap");
    }
}

TEST(VialZones, MovingHeadOntoAStillVialDoesNotInject) {
    Sim sim{};
    ExpectNoStab(sim.Play([](float t) {
        World w{};
        w.left = {-0.24f, 1.65f, 0.08f};
        w.eye.x = Path({{0.0f, {}}, {0.4f, {}}, {0.7f, {-0.20f, 0, 0}},
                       {1.2f, {-0.20f, 0, 0}}}, t).x;
        return w;
    }, 1.2f), "head moves onto still vial");
}

TEST(VialZones, NewTargetsDisableWithoutHeadTracking) {
    const auto geo = Z::Build({}, {0, 0, -1}, {}, {}, {});
    for (auto zone : {Z::Zone::Head, Z::Zone::LeftLeg, Z::Zone::RightLeg}) {
        EXPECT_FALSE(Z::MeasureZone(geo, zone, {}).valid);
    }
}


TEST(VialZones, NewTargetsWorkAcrossFrameRates) {
    for (float hz : {30.0f, 60.0f, 90.0f, 120.0f}) {
        Sim head{};
        head.dt = 1.0f / hz;
        ExpectOneStab(Stab(head, {0, 1.65f, -0.28f}, {0, 1.65f, -0.02f}),
                      {"head"}, "head at varied frame rates");
        Sim leg{};
        leg.dt = 1.0f / hz;
        leg.params.floor_known = true;
        ExpectOneStab(Stab(leg, {-0.12f, 0.3f, -0.27f}, {-0.12f, 0.3f, -0.04f}),
                      {"left leg"}, "leg at varied frame rates");
    }
}

TEST(VialZones, LookingDownTiltsHeadTargetWithoutMovingTheLegs) {
    Sim sim{};
    sim.params.floor_known = true;
    sim.Play([](float) { return World{}; }, 0.1f);
    const auto original = sim.geo;
    const Q look = B::AxisAngle({1, 0, 0}, -Pi / 3.0f);
    const V eye = original.neck + B::Rotate(look, {0, Z::NeckUp, -Z::NeckForward});
    sim.Play([=](float) { World w{}; w.head = look; w.eye = eye; return w; }, 0.1f);
    EXPECT_NEAR(B::Length(sim.geo.ankles[0] - original.ankles[0]), 0, 1e-5f);
    EXPECT_NEAR(B::Length(sim.geo.head_top - original.neck -
                         B::Rotate(look, {0, 0.17f, 0})), 0, 1e-5f);
    const V target = (sim.geo.head_base + sim.geo.head_top) * 0.5f + V{-0.09f, 0, 0};
    World w{}; w.head = look; w.eye = eye;
    ExpectOneStab(Stab(sim, target + V{-0.24f, 0, 0}, target, 0.3f, w),
                  {"head"}, "tilted head injection");
}


TEST(VialZones, UnsettledHeadContactExpiresInsteadOfInjectingLater) {
    for (float hz : {30.f, 60.f, 90.f, 120.f}) {
        Sim sim{};
        sim.dt = 1.f / hz;
        ExpectNoStab(sim.Play([](float t) {
            World w{};
            const V centre{-0.06f, 1.65f, 0.08f};
            if (t < .8f) {
                w.left = Path({{0, {-.36f, 1.65f, .08f}}, {.5f, {-.36f, 1.65f, .08f}},
                               {.8f, centre}}, t);
                // A brisk stroke continuing into a small circle, without a settled endpoint.
                if (t > .65f) { w.left_v_override = true; w.left_v = {.4f, 0, 0}; }
            } else {
                const float angle = std::min(t - .8f, .7f) * 18.f;
                w.left = centre + V{0, .025f * std::sin(angle), .025f * (std::cos(angle) - 1)};
            }
            return w;
        }, 2.f), "unsettled head contact then late stop");
    }
}

TEST(VialZones, WeaponSwingCancelsPendingHeadContact) {
    Sim sim{};
    ExpectNoStab(sim.Play([](float t) {
        World w{};
        w.left = Path({{0, {-.36f, 1.65f, .08f}}, {.5f, {-.36f, 1.65f, .08f}},
                       {.8f, {-.09f, 1.65f, .08f}}}, t);
        w.right = Path({{0, {.18f, 1.25f, -.4f}}, {.74f, {.18f, 1.25f, -.4f}},
                        {.94f, {.18f, 1.25f, -1.f}}}, t);
        return w;
    }, 1.5f), "weapon swing interrupts head injection gesture");
}


} // namespace

namespace {
namespace P = VR::VialContact;
P::Sample ContactSample(B::Vec3 point, std::int64_t ns) {
    P::Sample s{}; s.valid=true; s.ns=ns; s.space=7; s.tip=point;
    s.body.valid=true; s.body.ns=ns; s.body.space=7;
    s.body.p={{{0,1,0},{0,1.15f,0},{0,1.3f,0},{0,1.45f,0},{0,1.6f,0},{0,1.7f,0},
        {.18f,1.48f,0},{.35f,1.2f,0},{.45f,1,0},{.45f,.92f,0},
        {-.18f,1.48f,0},{-.35f,1.2f,0},
        {-.1f,.95f,0},{-.1f,.55f,0},{-.1f,.1f,0},{-.1f,.02f,-.08f},
        {.1f,.95f,0},{.1f,.55f,0},{.1f,.1f,0},{-.45f,1,0},{.1f,.02f,-.08f},
        {.44f,.93f,0},{.45f,.93f,0},{.46f,.93f,0},{.47f,.93f,0},{.42f,.97f,0},
        {-.42f,.97f,0}}};
    return s;
}
struct ContactTarget { const char* name; B::Vec3 centre, direction; float skin; };
const std::array<ContactTarget,23> Targets{{
    {"hand",{.45f,.94f,0},{0,0,-1},.035f},
    {"wrist controller clearance",{.45f,1,0},{0,0,-1},.075f},
    {"forearm",{.4f,1.1f,0},{0,0,-1},.045f},
    {"elbow",{.35f,1.2f,0},{0,0,-1},.055f},
    {"upper arm",{.265f,1.34f,0},{0,0,-1},.055f},
    {"shoulder front",{.18f,1.48f,0},{0,0,-1},.065f},
    {"shoulder top",{.18f,1.48f,0},{0,1,0},.065f},
    {"shoulder side",{.18f,1.48f,0},{1,0,0},.065f},
    {"chest",{0,1.375f,0},{0,0,-1},.11f},
    {"belly",{0,1.225f,0},{0,0,-1},.10f},
    {"left side",{0,1.375f,0},{-1,0,0},.17f},
    {"right side",{0,1.375f,0},{1,0,0},.17f},
    {"hips",{0,1.025f,0},{0,0,-1},.10f},
    {"left thigh",{-.1f,.75f,0},{0,0,-1},.075f},
    {"right thigh",{.1f,.75f,0},{0,0,-1},.075f},
    {"left knee",{-.1f,.55f,0},{0,0,-1},.075f},
    {"right knee",{.1f,.55f,0},{0,0,-1},.075f},
    {"left shin",{-.1f,.325f,0},{0,0,-1},.055f},
    {"right shin",{.1f,.325f,0},{0,0,-1},.055f},
    {"headset front",{0,1.7f,0},{0,0,-1},.12f},
    {"headset right",{0,1.7f,0},{1,0,0},.12f},
    {"headset left",{0,1.7f,0},{-1,0,0},.12f},
    {"headset top",{0,1.7f,0},{0,1,0},.12f},
}};
}
TEST(VialContact, EveryTargetWorksWithoutPenetrationStandingAndSeated) {
    for(const auto& target:Targets) for(float hz:{30.f,60.f,72.f,80.f,90.f,120.f})
    for(float speed:{.3f,1.f}) for(float seat:{0.f,-.45f}) {
        P::Gate gate; int hits=0;
        const float duration=.18f/speed;
        for(int i=0;i<int((duration+.7f)*hz);++i) {
            const auto ns=1'000'000'000+std::int64_t(float(i)*1e9/hz);
            const float remaining=std::max(0.f,.18f-speed*float(i)/hz);
            auto s=ContactSample(target.centre+target.direction*(target.skin+remaining),ns);
            const std::string_view name=target.name;
            const bool torso=name=="chest" || name=="belly" || name=="left side" ||
                             name=="right side" || name=="hips";
            if(torso) {
                // A side-of-torso test must not stop on the intervening upper arm.
                for(int j:{7,11}) s.body.p[j].y+=.7f;
                for(int j:{8,9,19,21,22,23,24,25,26}) s.body.p[j].y+=1.f;
            }
            s.tip.y+=seat; for(auto& p:s.body.p) p.y+=seat;
            const auto result=gate.Update(s,ns,7,true,false);
            if(result.stab) {
                ++hits;
                if(torso) EXPECT_EQ(result.hit.zone,Z::Zone::Body)<<target.name;
                if(name.starts_with("headset")) EXPECT_EQ(result.hit.zone,Z::Zone::Head);
                if(name=="hand") EXPECT_EQ(result.hit.zone,Z::Zone::Wrist);
                if(name.starts_with("wrist")) {
                    // The forearm ends at the wrist. A fast sample can enter both
                    // padded capsules together; either names the same contact.
                    EXPECT_TRUE(result.hit.zone==Z::Zone::Wrist ||
                                result.hit.zone==Z::Zone::Forearm)<<target.name;
                }
                if(name=="forearm") EXPECT_EQ(result.hit.zone,Z::Zone::Forearm);
                if(name.starts_with("shoulder") || name=="upper arm")
                    EXPECT_EQ(result.hit.zone,Z::Zone::UpperArm);
                if(name=="left thigh" || name=="left knee" || name=="left shin")
                    EXPECT_EQ(result.hit.zone,Z::Zone::LeftLeg);
                if(name=="right thigh" || name=="right knee" || name=="right shin")
                    EXPECT_EQ(result.hit.zone,Z::Zone::RightLeg);
                bool nonpositive=false;
                for(const auto& m:result.m) nonpositive|=m.valid && m.d<=1e-6f;
                EXPECT_TRUE(nonpositive)<<target.name;
            }
        }
        EXPECT_EQ(hits,1)<<target.name<<" Hz "<<hz<<" speed "<<speed<<" seated "<<(seat!=0);
    }
}
TEST(VialContact, RestingAtEveryTargetNeverInjects) {
    for(const auto& target:Targets) {
        P::Gate gate;
        for(int i=0;i<100;++i) {
            auto s=ContactSample(target.centre+target.direction*target.skin,1'000'000'000+i*11'111'111);
            EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab)<<target.name;
        }
    }
}
TEST(VialContact, PassingOutsideEveryTargetDoesNotInject) {
    for(const auto& target:Targets) {
        P::Gate gate;
        const auto tangent=B::Normalize(B::Cross(target.direction,{0,1,0}),{0,0,1});
        for(int i=0;i<50;++i) {
            // Tangent pass in empty space, beyond the largest controller allowance.
            auto s=ContactSample(target.centre+target.direction*(target.skin+.15f)+
                                  tangent*(-.05f+.1f*float(i)/49),1'000'000'000+i*11'111'111);
            EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab)<<target.name;
        }
    }
}
TEST(VialContact, HoldingArmIsNotATarget) {
    for(int a:{10,11,19}) {
        P::Gate gate;
        for(int i=0;i<40;++i) {
            auto s=ContactSample({0,0,0},1'000'000'000+i*16'666'667);
            s.tip=s.body.p[a]+B::Vec3{0,0,-.20f+.20f*float(i)/39};
            // Keep the left shoulder away from the torso to isolate the holding arm.
            s.body.p[a].x-=.4f; s.tip.x-=.4f;
            EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab);
        }
    }
}
TEST(VialContact, StationaryHandCannotBeInjectedByBodyHeadOrWeapon) {
    for(const auto& target:Targets) {
        P::Gate gate;
        for(int i=0;i<60;++i) {
            auto s=ContactSample(target.centre+target.direction*(target.skin+.14f),
                                  1'000'000'000+i*11'111'111);
            for(auto& p:s.body.p) p=p+target.direction*(.20f*float(i)/59);
            EXPECT_FALSE(gate.Update(s,s.ns,7,true,true).stab)<<target.name;
        }
    }
}
TEST(VialContact, MovingWholeBodyWithHandDoesNotInject) {
    P::Gate gate;
    for(int i=0;i<80;++i) {
        auto s=ContactSample({.45f,1,-.09f},1'000'000'000+i*11'111'111);
        const B::Vec3 shift{-.015f*i,0,0}; s.tip=s.tip+shift;
        for(auto& p:s.body.p) p=p+shift;
        EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab);
    }
}
TEST(VialContact, ShortDropoutKeepsOutsideTargetsArmed) {
    P::Gate gate;
    auto s=ContactSample({.45f,1,-.2f},1'000'000'000);
    EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab);
    P::Sample invalid{};
    EXPECT_FALSE(gate.Update(invalid,s.ns+100'000'000,7,true,false).stab);
    s=ContactSample({.45f,1,-.2f},1'150'000'000);
    EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab);
    s=ContactSample({.45f,1,-.075f},1'200'000'000);
    EXPECT_TRUE(gate.Update(s,s.ns,7,true,false).stab);
}
TEST(VialContact, LongDropoutOrRecenterDoesNotSweepAcrossMissingData) {
    for(bool recenter:{false,true}) {
        P::Gate gate;
        auto s=ContactSample({.45f,1,-.2f},1'000'000'000);
        gate.Update(s,s.ns,7,true,false);
        s=ContactSample({.45f,1,-.075f},1'300'000'000);
        if(recenter) s.space=s.body.space=8;
        EXPECT_FALSE(gate.Update(s,s.ns,s.space,true,false).stab);
    }
}
TEST(VialContact, DrawingInsideOneTargetDoesNotDisarmOthers) {
    P::Gate gate;
    auto s=ContactSample({.45f,1,-.04f},1'000'000'000);
    gate.Update(s,s.ns,7,true,false);
    int hits=0;
    for(int i=0;i<45;++i) {
        s=ContactSample(B::Vec3{.45f,1,-.04f}+B::Vec3{-.45f,.375f,-.07f}*(float(i)/44),
                        1'010'000'000+i*11'111'111);
        hits+=gate.Update(s,s.ns,7,true,false).stab;
    }
    EXPECT_EQ(hits,1);
}
TEST(VialContact, HeadsetRateTipMovesBetweenGamePublications) {
    auto source=ContactSample({0,0,0},1'000'000'000);
    source.grip_local=true; source.grip_offset={.02f,-.10f,.01f};
    const auto q=B::AxisAngle({0,1,0},1.2f);
    P::Gate gate; int hits=0;
    for(int i=0;i<20;++i) {
        const auto now=1'000'000'000+i*8'333'333;
        const B::Vec3 hub{.45f,1,-.20f+.125f*std::min(float(i)/12,1.f)};
        const auto sample=P::AtGrip(source,source.body,hub-B::Rotate(q,source.grip_offset),q,now,7);
        EXPECT_NEAR(B::Length(sample.tip-hub),0,1e-6f);
        hits+=gate.Update(sample,now,7,true,false).stab;
    }
    EXPECT_EQ(hits,1);
    EXPECT_FALSE(P::AtGrip(source,source.body,{},q,1'300'000'000,7).valid);
}
TEST(VialContact, FreshSampleRejectsInvalidSpaceAndOldBody) {
    auto s=ContactSample({.45f,1,-.2f},1'000'000'000);
    EXPECT_TRUE(P::Fresh(s,s.ns,7));
    EXPECT_FALSE(P::Fresh(s,s.ns,8));
    EXPECT_FALSE(P::Fresh(s,s.ns+P::GraceNs+1,7));
    s.body.ns=0; EXPECT_FALSE(P::Fresh(s,s.ns,7));
}

TEST(VialContact, NewDrawKeepsCooldownAndAllowsOnlyOneRequest) {
    P::Gate gate;
    auto start=ContactSample({.45f,1,-.2f},1'000'000'000);
    gate.Update(start,start.ns,7,true,false);
    auto touch=ContactSample({.45f,1,-.075f},1'100'000'000);
    ASSERT_TRUE(gate.Update(touch,touch.ns,7,true,false).stab);
    gate.StartMoved({});
    start.ns=start.body.ns=1'200'000'000;
    gate.Update(start,start.ns,7,true,false);
    touch.ns=touch.body.ns=1'250'000'000;
    EXPECT_FALSE(gate.Update(touch,touch.ns,7,true,false).stab);
    gate.StartMoved({});
    start.ns=start.body.ns=1'700'000'000;
    gate.Update(start,start.ns,7,true,false);
    touch.ns=touch.body.ns=1'800'000'000;
    EXPECT_TRUE(gate.Update(touch,touch.ns,7,true,false).stab);
}

TEST(VialContact, ExistingZoneScaleControlsControllerAllowance) {
    for(float margin_scale:{.5f,1.f,2.f}) {
        P::Gate gate; int hits=0;
        for(int i=0;i<40;++i) {
            auto s=ContactSample({.45f,.96f,-std::max(.075f,.22f-.005f*i)},
                                  1'000'000'000+i*16'666'667);
            hits+=gate.Update(s,s.ns,7,true,false,margin_scale).stab;
        }
        EXPECT_EQ(hits,margin_scale>=1.f?1:0);
    }
}

TEST(VialContact, LiveRightGripMovesHandAndPartiallyMovesElbowWithShoulderAnchored) {
    auto source=ContactSample({},1'000'000'000);
    source.body.right_grip_valid=true;
    source.body.right_grip_p={.45f,1,0};
    source.body.right_grip_q=B::AxisAngle({0,1,0},.4f);
    auto body=source.body;
    const B::Vec3 live{.55f,1.1f,-.2f};
    const auto q=B::AxisAngle({0,1,0},1.2f);
    P::AtRightGrip(body,live,q,1'011'111'111);
    for(int i:{8,9,21,22,23,24,25}) {
        const auto local=B::Rotate(B::Conjugate(source.body.right_grip_q),
                                   source.body.p[i]-source.body.right_grip_p);
        EXPECT_NEAR(B::Length(body.p[i]-(live+B::Rotate(q,local))),0,1e-6f);
        EXPECT_EQ(body.point_ns[i],1'011'111'111);
    }
    EXPECT_NEAR(B::Length(body.p[6]-source.body.p[6]),0,1e-6f);
    EXPECT_EQ(body.point_ns[6],source.body.point_ns[6]);
    EXPECT_NEAR(B::Length(body.p[7]-source.body.p[7]-
                         (body.p[8]-source.body.p[8])*.5f),0,1e-6f);
    EXPECT_EQ(body.point_ns[7],1'011'111'111);
    EXPECT_EQ(body.ns,source.body.ns);
    EXPECT_NEAR(B::Length(body.p[3]-source.body.p[3]),0,1e-6f);
}
TEST(VialContact, SeatedThighAndKneeSurfaceTouchesAheadOfStandingSkeleton) {
    Z::Geometry g{}; g.body_ok=true; g.params.seated=true;
    g.hips={B::Vec3{-.12f,.5f,0},B::Vec3{.12f,.5f,0}};
    g.knees={B::Vec3{-.12f,.5f,-.5f},B::Vec3{.12f,.5f,-.5f}};
    for(int side:{0,1}) for(float forward:{.25f,.5f}) for(float speed:{.3f,1.f}) {
        P::Gate gate; int hits=0;
        for(int i=0;i<150;++i) {
            const float extra=std::max(0.f,.18f-speed*i/90.f);
            auto s=ContactSample({side==0?-.12f:.12f,.58f+extra,-forward},
                                  1'000'000'000+i*11'111'111);
            // The hunter still has straight standing legs, well behind the real lap.
            P::SeatedLap(s.body,g,s.ns);
            const auto r=gate.Update(s,s.ns,7,true,false);
            if(r.stab) { ++hits; EXPECT_EQ(r.hit.zone,Z::Zone::Lap); }
        }
        EXPECT_EQ(hits,1);
    }
}
TEST(VialContact, AnatomyVelocityUsesGameAndLivePointClocksIndependently) {
    P::AnatomyVelocity velocity;
    auto s=ContactSample({},1'000'000'000);
    velocity.Update(s.body,true);
    for(int frame=1;frame<=18;++frame) {
        // 90 Hz XR, 30 Hz game; skip one whole game publication as well.
        const auto game_frame=frame<9?(frame/3)*3:(frame<12?6:(frame/3)*3);
        auto b=s.body;
        b.ns=s.body.ns+game_frame*11'111'111;
        b.p[4].x+=.3f*game_frame/90.f;
        b.point_ns[8]=s.body.ns+frame*11'111'111;
        b.p[8].x+=.6f*frame/90.f;
        velocity.Update(b,false);
        if(game_frame>=3) EXPECT_NEAR(velocity[4].x,.3f,1e-5f);
        EXPECT_NEAR(velocity[8].x,.6f,1e-5f);
    }
}
TEST(VialContact, ContactMarginsAndHeadsetStayInRoomMetresAtDifferentBodyScales) {
    for(float scale:{.5f,1.f,2.f}) {
        auto head=ContactSample({0,1.7f,-.12f},1'000'000'000);
        head.body.scale=scale;
        P::Gate head_gate;
        auto r=head_gate.Update(head,head.ns,7,true,false);
        EXPECT_NEAR(r.m[static_cast<int>(Z::Zone::Head)].d,0,1e-6f);
        auto wrist=ContactSample({.45f,1,-(.035f*scale+.05f)},1'000'000'000);
        wrist.body.scale=scale;
        P::Gate wrist_gate;
        r=wrist_gate.Update(wrist,wrist.ns,7,true,false);
        EXPECT_NEAR(r.m[static_cast<int>(Z::Zone::Wrist)].d,0,1e-6f);
    }
}
TEST(VialContact, LiveWeaponArmCannotSwingOntoAStillSyringe) {
    P::Gate gate;
    auto source=ContactSample({.45f,1,-.2f},1'000'000'000);
    source.body.right_grip_valid=true; source.body.right_grip_p={.45f,1,0};
    for(int i=0;i<20;++i) {
        auto s=source; s.ns+=i*11'111'111;
        P::AtRightGrip(s.body,{.45f,1,-.015f*i},{},s.ns);
        EXPECT_FALSE(gate.Update(s,s.ns,7,true,true).stab);
    }
}

TEST(VialContact, SeatedLapDoesNotFireAtRestOrDuringANearbyPass) {
    Z::Geometry g{}; g.body_ok=true; g.params.seated=true;
    g.hips={B::Vec3{-.12f,.5f,0},B::Vec3{.12f,.5f,0}};
    g.knees={B::Vec3{-.12f,.5f,-.5f},B::Vec3{.12f,.5f,-.5f}};
    for(bool pass:{false,true}) for(int side:{0,1}) {
        P::Gate gate;
        for(int i=0;i<90;++i) {
            auto s=ContactSample({side==0?-.12f:.12f,pass?.66f:.58f,
                                 pass?-.1f-.4f*i/90.f:-.3f},1'000'000'000+i*11'111'111);
            P::SeatedLap(s.body,g,s.ns);
            EXPECT_FALSE(gate.Update(s,s.ns,7,true,false).stab);
        }
    }
}

TEST(VialContact, ThirtyDegreeWristRotationKeepsShoulderAndChestShape) {
    for(const B::Vec3 axis : {B::Vec3{1,0,0},B::Vec3{0,1,0},B::Vec3{0,0,1}}) {
        auto source=ContactSample({},1'000'000'000);
        source.body.right_grip_valid=true;
        source.body.right_grip_p=source.body.p[8];
        auto moved=source;
        P::AtRightGrip(moved.body,source.body.right_grip_p,
                      B::AxisAngle(axis,3.14159265f/6),1'011'111'111);
        EXPECT_NEAR(B::Length(moved.body.p[6]-source.body.p[6]),0,1e-6f);
        EXPECT_NEAR(B::Length(moved.body.p[7]-source.body.p[7]),0,1e-6f);
        const auto before=P::GetTorsoAxes(source.body),after=P::GetTorsoAxes(moved.body);
        EXPECT_NEAR(B::Length(after.x-before.x),0,1e-6f);
        EXPECT_NEAR(B::Length(after.y-before.y),0,1e-6f);
        EXPECT_NEAR(B::Length(after.z-before.z),0,1e-6f);
        // Probe the actual chest/belly/hip collision shape from front and sides.
        for(float y:{1.025f,1.225f,1.375f}) for(B::Vec3 dir:
            {B::Vec3{1,0,0},B::Vec3{-1,0,0},B::Vec3{0,0,-1}}) {
            source.tip=moved.tip=B::Vec3{0,y,0}+dir*.2f;
            P::Gate a,b;
            const auto ra=a.Update(source,source.ns,7,true,false);
            const auto rb=b.Update(moved,moved.ns,7,true,false);
            const int zone=static_cast<int>(Z::Zone::Body);
            EXPECT_FLOAT_EQ(ra.m[zone].d,rb.m[zone].d);
        }
    }
}
TEST(VialContact, FrozenAdjustedGripKeepsArmTargetsAtVisiblePose) {
    auto source=ContactSample({},1'000'000'000);
    source.body.right_grip_valid=true;
    source.body.right_grip_p={.45f,1,0};
    source.body.right_grip_q=B::AxisAngle({0,1,0},.7f);
    auto body=source.body;
    // Published hand stays frozen even if the raw controller moves away.
    P::AtRightGrip(body,source.body.right_grip_p,source.body.right_grip_q,1'011'111'111);
    for(int i:{6,7,8,9,21,22,23,24,25})
        EXPECT_NEAR(B::Length(body.p[i]-source.body.p[i]),0,1e-6f);
}
