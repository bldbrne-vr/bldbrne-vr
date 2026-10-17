// SPDX-License-Identifier: GPL-2.0-or-later

// Physical viscerals (core/vr/visceral_thrust.h): the thrust detector on synthetic right-hand
// paths sampled like the OpenXR frame loop (90 Hz; velocities by central differences, as the
// runtime's), the verdict of a melee window, and the heartbeat cue. OpenXR LOCAL space: the head
// at (0, 1.6, 0) facing -Z, the right shoulder at (0.18, 1.38, 0.05).

#include <cmath>
#include <functional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/visceral_thrust.h"

namespace {

namespace M = VR::Melee;
namespace T = VR::Melee::Thrust;

constexpr float Pi = 3.14159265f;
constexpr float Rad = Pi / 180.0f;
constexpr double Hz = 90.0;
const M::V3 Shoulder{0.18f, 1.38f, 0.05f};

// Minimum jerk 0 -> 1 over [0, 1].
float MinJerk(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * x * (10.0f - 15.0f * x + 6.0f * x * x);
}

// A blade direction: yaw (+ = left of -Z) and pitch (+ = up) in degrees.
M::V3 Dir(float yaw_deg, float pitch_deg) {
    const float y = yaw_deg * Rad, p = pitch_deg * Rad;
    return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

struct Sample {
    M::V3 hand{};
    M::V3 blade{};
    M::V3 head{0.0f, 1.6f, 0.0f};
};
using Path = std::function<Sample(double)>;

struct ThrustRun {
    std::vector<double> fires; // times of the thrusts
    std::vector<T::Output> outs;
    std::vector<double> times;
    const char* last_why{""};
    T::Verdict first_verdict_after(double t0) const {
        for (size_t i = 0; i < outs.size(); ++i) {
            if (times[i] >= t0 && outs[i].verdict != T::Verdict::Pending &&
                outs[i].verdict != T::Verdict::None) {
                return outs[i].verdict;
            }
        }
        return T::Verdict::None;
    }
    double decided_at(T::Verdict v) const {
        for (size_t i = 0; i < outs.size(); ++i) {
            if (outs[i].verdict == v) {
                return times[i];
            }
        }
        return -1.0;
    }
};

ThrustRun Drive(const Path& path, double seconds, const T::Config& cfg = T::Config{},
          bool active = true) {
    T::Detector d;
    ThrustRun r;
    const double h = 1.0 / Hz;
    for (double t = 0.0; t <= seconds; t += h) {
        const Sample a = path(t - 0.004), b = path(t + 0.004), s = path(t);
        T::Input in{};
        in.t = t;
        in.active = active;
        in.valid = true;
        in.v = (b.hand - a.hand) * (1.0f / 0.008f);
        in.head_v = (b.head - a.head) * (1.0f / 0.008f);
        in.head_ok = true;
        in.head_fwd = {0.0f, 0.0f, -1.0f};
        in.blade = s.blade;
        const T::Output o = d.Update(in, cfg);
        if (o.fired) {
            r.fires.push_back(t);
        }
        if (o.why != nullptr && o.why[0] != 0) {
            r.last_why = o.why;
        }
        r.outs.push_back(o);
        r.times.push_back(t);
    }
    return r;
}

// The hand straight along `dir` by `len` m, minimum jerk peaking at `peak` m/s, from t0; the
// blade along `blade` (default: dir).
Path Straight(M::V3 start, M::V3 dir, float len, float peak, double t0, M::V3 blade = {}) {
    const float T_ = 1.875f * len / peak;
    if (M::Length(blade) < 0.5f) {
        blade = dir;
    }
    return [=](double t) {
        Sample s;
        const float k = MinJerk(static_cast<float>((t - t0) / T_));
        s.hand = start + dir * (len * k);
        s.blade = blade;
        return s;
    };
}

// The hand on a 0.55 m sphere round the shoulder, the blade out along the radius, from
// (yaw0, pitch0) to (yaw1, pitch1), peaking at `peak` m/s (a swing like the mock runtime's).
// `lead_deg`: the blade that far ahead of the arm's line along the swing.
Path Arc(float yaw0, float pitch0, float yaw1, float pitch1, float peak, double t0,
         float lead_deg = 0.0f) {
    const M::V3 a = Dir(yaw0, pitch0), b = Dir(yaw1, pitch1);
    const float angle = M::Angle(a, b);
    const float T_ = 1.875f * angle * 0.55f / peak;
    // slerp of directions
    const auto at = [=](float k) {
        return M::Normalize(a * (std::sin((1.0f - k) * angle) / std::sin(angle)) +
                            b * (std::sin(k * angle) / std::sin(angle)));
    };
    return [=](double t) {
        const float k = MinJerk(static_cast<float>((t - t0) / T_));
        Sample s;
        s.hand = Shoulder + at(k) * 0.55f;
        s.blade = at(k + lead_deg * Rad / angle);
        return s;
    };
}

const M::V3 Fwd = Dir(0.0f, 0.0f);
const M::V3 Start = Shoulder + Fwd * 0.20f;

} // namespace

TEST(VisceralThrust, ThrustFiresOnceEarly) {
    const ThrustRun r = Drive(Straight(Start, Fwd, 0.35f, 3.0f, 0.2), 1.2);
    ASSERT_EQ(r.fires.size(), 1u);
    // 12 cm of 35: about 40% into the 0.22 s stroke.
    EXPECT_LT(r.fires[0] - 0.2, 0.13);
    EXPECT_GT(r.fires[0] - 0.2, 0.04);
}

TEST(VisceralThrust, MockStabFires) {
    const ThrustRun r = Drive(Straight(Start, Fwd, 0.35f, 6.0f, 0.2), 1.0);
    ASSERT_EQ(r.fires.size(), 1u);
    EXPECT_LT(r.fires[0] - 0.2, 0.07);
}

TEST(VisceralThrust, ShortQuickThrustFires) {
    // 18 cm at 2.2 m/s: a short deliberate jab.
    const ThrustRun r = Drive(Straight(Start, Fwd, 0.18f, 2.2f, 0.2), 1.0);
    EXPECT_EQ(r.fires.size(), 1u);
}

TEST(VisceralThrust, SlowPushDoesNot) {
    const ThrustRun r = Drive(Straight(Start, Fwd, 0.35f, 1.2f, 0.2), 1.5);
    EXPECT_EQ(r.fires.size(), 0u);
}

TEST(VisceralThrust, ForehandSwingDoesNot) {
    const ThrustRun r = Drive(Arc(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f, 0.2), 1.2);
    EXPECT_EQ(r.fires.size(), 0u);
    // A swing quickly: the window of a swing at an open visceral waits at most this long.
    const double at = r.decided_at(T::Verdict::Swing);
    EXPECT_GE(at, 0.2);
    EXPECT_LT(at - 0.2, 0.09);
}

TEST(VisceralThrust, BackhandAndChopDoNot) {
    EXPECT_EQ(Drive(Arc(55.0f, 0.0f, -60.0f, -5.0f, 5.0f, 0.2), 1.2).fires.size(), 0u);
    EXPECT_EQ(Drive(Arc(0.0f, 130.0f, 0.0f, -40.0f, 6.0f, 0.2), 1.2).fires.size(), 0u);
    // A slow small swing too.
    EXPECT_EQ(Drive(Arc(-30.0f, 0.0f, 20.0f, 0.0f, 2.5f, 0.2), 1.2).fires.size(), 0u);
}

TEST(VisceralThrust, SwingThatStartsAlongTheBladeDoesNot) {
    // A swing whose first frames move along the blade (an arc from straight ahead outward,
    // pivoting at the elbow 30 cm behind the hand): the blade turns past 30 degrees.
    const M::V3 elbow = Shoulder + Fwd * 0.25f;
    const Path p = [=](double t) {
        const float k = MinJerk(static_cast<float>((t - 0.2) / 0.18));
        const M::V3 d = Dir(-80.0f * k, 0.0f);
        Sample s;
        s.hand = elbow + d * 0.30f + Fwd * (0.10f * k);
        s.blade = d;
        return s;
    };
    EXPECT_EQ(Drive(p, 1.0).fires.size(), 0u);
}

TEST(VisceralThrust, WindUpThenThrustFiresOnceOnTheThrust) {
    // Pulled back 25 cm along the blade (2.5 m/s), straight into a 35 cm thrust.
    const M::V3 back_start = Start + Fwd * 0.15f;
    const float t_back = 1.875f * 0.25f / 2.5f;
    const Path pull = Straight(back_start, Fwd * -1.0f, 0.25f, 2.5f, 0.2, Fwd);
    const Path thrust = Straight(back_start - Fwd * 0.25f, Fwd, 0.35f, 3.0f, 0.2 + t_back);
    const Path p = [=](double t) { return t < 0.2 + t_back ? pull(t) : thrust(t); };
    const ThrustRun r = Drive(p, 1.5);
    ASSERT_EQ(r.fires.size(), 1u);
    EXPECT_GT(r.fires[0], 0.2 + t_back);
}

TEST(VisceralThrust, PullBackAloneDoesNot) {
    const ThrustRun r = Drive(Straight(Start + Fwd * 0.2f, Fwd * -1.0f, 0.30f, 3.0f, 0.2, Fwd), 1.2);
    EXPECT_EQ(r.fires.size(), 0u);
}

TEST(VisceralThrust, RaisingTheWeaponDoesNot) {
    const M::V3 up = Dir(0.0f, 80.0f);
    const ThrustRun r = Drive(Straight(Shoulder + Fwd * 0.3f, up, 0.30f, 3.0f, 0.2), 1.2);
    EXPECT_EQ(r.fires.size(), 0u);
}

TEST(VisceralThrust, LoweredOrLevelEnough) {
    // 30 degrees down at a crouching enemy: still a thrust; 70 degrees down: not.
    EXPECT_EQ(Drive(Straight(Start, Dir(0.0f, -30.0f), 0.30f, 3.0f, 0.2), 1.0).fires.size(), 1u);
    EXPECT_EQ(Drive(Straight(Start, Dir(0.0f, -70.0f), 0.30f, 3.0f, 0.2), 1.0).fires.size(), 0u);
}

TEST(VisceralThrust, SidewaysOrBehindDoesNot) {
    EXPECT_EQ(Drive(Straight(Start, Dir(-90.0f, 0.0f), 0.30f, 3.0f, 0.2), 1.0).fires.size(), 0u);
    EXPECT_EQ(Drive(Straight(Start, Dir(150.0f, 0.0f), 0.30f, 3.0f, 0.2), 1.0).fires.size(), 0u);
    // 45 degrees to the side: still ahead enough.
    EXPECT_EQ(Drive(Straight(Start, Dir(45.0f, 0.0f), 0.30f, 3.0f, 0.2), 1.0).fires.size(), 1u);
}

TEST(VisceralThrust, MotionOffTheBladeByTheHandsPath) {
    // The hand stabs straight ahead with the blade off its line (a wrist turned 55 degrees, a
    // hammer grip 50 degrees up): the hand's path makes it a thrust.
    for (const M::V3 blade : {Dir(55.0f, 0.0f), Dir(-55.0f, 0.0f), Dir(0.0f, 50.0f)}) {
        const ThrustRun r = Drive(Straight(Start, Fwd, 0.30f, 2.5f, 0.2, blade), 1.0);
        ASSERT_EQ(r.fires.size(), 1u);
        EXPECT_STREQ(r.last_why, "thrust (the hand's path)");
    }
    // The blade 70 degrees off where the head faces (a slash-push sideways): not ahead.
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.35f, 3.0f, 0.2, Dir(70.0f, 0.0f)), 1.0).fires.size(),
              0u);
    // The hand 40 degrees off the facing with the blade off its line: no thrust by the path.
    EXPECT_EQ(Drive(Straight(Start, Dir(40.0f, 0.0f), 0.30f, 2.5f, 0.2, Dir(-20.0f, 0.0f)), 1.0)
                  .fires.size(),
              0u);
    // The blade 65 degrees up off a level stab (more than path_blade_max): not by the path.
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.30f, 2.5f, 0.2, Dir(0.0f, 65.0f)), 1.0).fires.size(),
              0u);
}

TEST(VisceralThrust, HandsPathHoldsUpUnderTrackingNoise) {
    // The runtime's hand velocity jitters (here 3 cm/s per axis, Gaussian): the path's bend is
    // judged against its chord, so the noise does not add up frame after frame.
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 0.03f);
    int fired = 0;
    for (int run = 0; run < 100; ++run) {
        const Path p = Straight(Start, Fwd, 0.25f, 2.0f, 0.2, Dir(55.0f, 0.0f));
        T::Detector d;
        for (double t = 0.0; t <= 1.0; t += 1.0 / Hz) {
            const Sample a = p(t - 0.004), b = p(t + 0.004);
            T::Input in{};
            in.t = t;
            in.active = in.valid = in.head_ok = true;
            in.v = (b.hand - a.hand) * (1.0f / 0.008f) + M::V3{noise(rng), noise(rng), noise(rng)};
            in.blade = p(t).blade;
            fired += d.Update(in, T::Config{}).fired ? 1 : 0;
        }
    }
    EXPECT_GE(fired, 95);
}

TEST(VisceralThrust, SweepsAreNeverThrusts) {
    // Arcs round the shoulder from either side, high to low and low to high, 60 to 130 degrees,
    // at 1.8 to 6 m/s, the blade along the arm or leading it by 20 degrees: the hand's path bends
    // and the blade turns off the motion.
    int thrusts = 0;
    for (const float peak : {1.8f, 2.5f, 3.2f, 4.0f, 6.0f}) {
        for (const float y0 : {-80.0f, -60.0f, -40.0f, 40.0f, 60.0f, 80.0f}) {
            for (const float p0 : {70.0f, 40.0f, 10.0f, -30.0f}) {
                for (const float span : {60.0f, 90.0f, 130.0f}) {
                    const float y1 = y0 > 0.0f ? y0 - span : y0 + span;
                    const float p1 = p0 > 20.0f ? p0 - 70.0f : p0 + 20.0f;
                    thrusts += static_cast<int>(
                        Drive(Arc(y0, p0, y1, p1, peak, 0.2), 1.4).fires.size());
                    thrusts += static_cast<int>(
                        Drive(Arc(y0, p0, y1, p1, peak, 0.2, 20.0f), 1.4).fires.size());
                }
            }
        }
    }
    EXPECT_EQ(thrusts, 0);
}

TEST(VisceralThrust, StepForwardWithTheHandStillDoesNot) {
    // The whole body (head and hand) moves 50 cm forward at 2 m/s.
    const Path body = Straight({0.0f, 0.0f, 0.0f}, Fwd, 0.5f, 2.0f, 0.2);
    const Path p = [=](double t) {
        Sample s = body(t);
        const M::V3 off = s.hand;
        s.hand = Start + off;
        s.head = M::V3{0.0f, 1.6f, 0.0f} + off;
        s.blade = Fwd;
        return s;
    };
    EXPECT_EQ(Drive(p, 1.2).fires.size(), 0u);
}

TEST(VisceralThrust, WaggleAlongTheBladeDoesNot) {
    const Path p = [](double t) {
        Sample s;
        s.hand = Start + Fwd * (0.04f * std::sin(static_cast<float>(2.0 * 3.14159 * 4.0 * t)));
        s.blade = Fwd;
        return s;
    };
    EXPECT_EQ(Drive(p, 2.0).fires.size(), 0u);
}

TEST(VisceralThrust, OffWhileSwingsAreOff) {
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.35f, 3.0f, 0.2), 1.0, T::Config{}, false).fires.size(),
              0u);
}

TEST(VisceralThrust, RefractoryAndSecondThrust) {
    // Two thrusts, the hand back between them (slowly), 0.25 s and 0.9 s apart.
    for (const double gap : {0.25, 0.9}) {
        const Path a = Straight(Start, Fwd, 0.30f, 3.0f, 0.2);
        const double t_end = 0.2 + 1.875 * 0.30 / 3.0;
        const Path back = Straight(Start + Fwd * 0.30f, Fwd * -1.0f, 0.30f, 0.35f, t_end + 0.05);
        const double t_back_end = t_end + 0.05 + 1.875 * 0.30 / 0.35;
        const Path b = Straight(Start, Fwd, 0.30f, 3.0f, t_back_end + gap);
        const Path p = [=](double t) {
            return t < t_end + 0.05 ? a(t) : t < t_back_end + gap ? back(t) : b(t);
        };
        const ThrustRun r = Drive(p, t_back_end + gap + 0.8);
        EXPECT_EQ(r.fires.size(), 2u); // the slow return (1.6 s) outlasts the refractory
    }
}

TEST(VisceralThrust, QuickSecondJabWithinTheRefractory) {
    // Two 15 cm jabs (3 m/s), the hand still 60 ms between them: the second within 0.35 s of the
    // first thrust is no other; 0.5 s later it is.
    for (const double pause : {0.06, 0.5}) {
        const float T1 = 1.875f * 0.15f / 3.0f;
        const Path a = Straight(Start, Fwd, 0.15f, 3.0f, 0.2);
        const Path b = Straight(Start + Fwd * 0.15f, Fwd, 0.15f, 3.0f, 0.2 + T1 + pause);
        const Path p = [=](double t) { return t < 0.2 + T1 + pause ? a(t) : b(t); };
        const ThrustRun r = Drive(p, 1.5);
        EXPECT_EQ(r.fires.size(), pause < 0.2 ? 1u : 2u);
    }
}

TEST(VisceralThrust, ConfigMakeClamps) {
    const T::Config c = T::Config::Make(0.1f, 100.0f, 90.0f, 1.0f);
    EXPECT_NEAR(c.min_speed, 0.8f, 1e-6f);
    EXPECT_NEAR(c.min_travel, 0.40f, 1e-6f);
    EXPECT_NEAR(c.align_cos, std::cos(60.0f * Rad), 1e-5f);
    EXPECT_NEAR(c.max_turn, 10.0f * Rad, 1e-5f);
    // A lower speed setting lets the slow push through.
    const T::Config slow = T::Config::Make(1.0f, 12.0f, 37.0f, 30.0f);
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.35f, 1.2f, 0.2), 1.5, slow).fires.size(), 1u);
}

// The feel review (2026-10-06): the stabs a player means, at human speeds (minimum jerk), fire with
// the defaults; the old 1.8 m/s / 12 cm / 37 deg missed them.
TEST(VisceralThrust, FeelReviewMeantStabsFire) {
    // A tentative 25 cm stab in 0.3 s (1.56 m/s peak), a seated 18 cm jab in 0.22 s (1.53).
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.25f, 1.5625f, 0.2), 1.2).fires.size(), 1u);
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.18f, 1.534f, 0.2), 1.2).fires.size(), 1u);
    // The blade held 40 degrees above the motion (a hammer grip pushed forward level).
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.30f, 2.5f, 0.2, Dir(0.0f, 40.0f)), 1.2).fires.size(),
              1u);
    // A lunge: the hand 45 cm in 0.3 s while the head comes 20 cm with it (1.56 m/s against it).
    const Path arm = Straight(Start, Fwd, 0.25f, 1.5625f, 0.2);
    const Path body = Straight({0.0f, 0.0f, 0.0f}, Fwd, 0.20f, 1.25f, 0.2);
    const Path lunge = [=](double t) {
        Sample s = arm(t);
        const M::V3 off = body(t).hand;
        s.hand = s.hand + off;
        s.head = s.head + off;
        return s;
    };
    EXPECT_EQ(Drive(lunge, 1.2).fires.size(), 1u);
    // The old values miss the tentative stab.
    const T::Config old = T::Config::Make(1.8f, 12.0f, 37.0f, 30.0f);
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.25f, 1.5625f, 0.2), 1.2, old).fires.size(), 0u);
}

TEST(VisceralThrust, FeelReviewReachIsNoThrust) {
    // Reaching forward 30 cm in 0.45 s (1.25 m/s) and a slow push stay no thrust.
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.30f, 1.25f, 0.2), 1.5).fires.size(), 0u);
    EXPECT_EQ(Drive(Straight(Start, Fwd, 0.35f, 1.2f, 0.2), 1.5).fires.size(), 0u);
}

// ---- WindowVerdict ------------------------------------------------------------------------------

namespace {

T::Output Det(T::Verdict v, std::uint32_t motion, bool fired = false, const char* why = "") {
    T::Output o{};
    o.verdict = v;
    o.motion = motion;
    o.fired = fired;
    o.why = why;
    return o;
}

} // namespace

TEST(VisceralThrustWindow, ThrustBeforeTheWindowIsTheThrusts) {
    T::WindowVerdict wv;
    const float dt = 1.0f / 90.0f;
    wv.Step(0, Det(T::Verdict::Pending, 3), dt);
    wv.Step(0, Det(T::Verdict::Thrust, 3, true), dt);
    for (int i = 0; i < 3; ++i) {
        wv.Step(0, Det(T::Verdict::Thrust, 3), dt);
    }
    wv.Step(7, Det(T::Verdict::Thrust, 3), dt); // the window opens at the commit, 40 ms later
    EXPECT_EQ(wv.window, 7u);
    EXPECT_TRUE(wv.verdict == T::Verdict::Thrust);
}

TEST(VisceralThrustWindow, OldThrustDoesNotCount) {
    T::WindowVerdict wv;
    const float dt = 1.0f / 90.0f;
    wv.Step(0, Det(T::Verdict::Thrust, 3, true), dt);
    for (int i = 0; i < 40; ++i) {
        wv.Step(0, Det(T::Verdict::None, 3), dt);
    }
    wv.Step(8, Det(T::Verdict::Pending, 4), dt); // a new motion's window
    EXPECT_TRUE(wv.verdict == T::Verdict::Pending);
    wv.Step(8, Det(T::Verdict::Swing, 4, false, "the blade turned (a swing)"), dt);
    EXPECT_TRUE(wv.verdict == T::Verdict::Swing);
    EXPECT_STREQ(wv.why, "the blade turned (a swing)");
}

TEST(VisceralThrustWindow, ThrustDuringTheWindow) {
    T::WindowVerdict wv;
    const float dt = 1.0f / 90.0f;
    wv.Step(9, Det(T::Verdict::Pending, 5), dt);
    EXPECT_TRUE(wv.verdict == T::Verdict::Pending);
    wv.Step(9, Det(T::Verdict::Thrust, 5, true), dt);
    EXPECT_TRUE(wv.verdict == T::Verdict::Thrust);
    // It stays the thrust's when the window closes.
    wv.Step(0, Det(T::Verdict::None, 5), dt);
    EXPECT_TRUE(wv.verdict == T::Verdict::Thrust);
}

TEST(VisceralThrustWindow, NoMotionOrClosedIsASwing) {
    const float dt = 1.0f / 90.0f;
    T::WindowVerdict a;
    a.Step(10, Det(T::Verdict::None, 5), dt);
    EXPECT_TRUE(a.verdict == T::Verdict::Pending);
    for (int i = 0; i < 6; ++i) {
        a.Step(10, Det(T::Verdict::None, 5), dt);
    }
    EXPECT_TRUE(a.verdict == T::Verdict::Swing); // a wrist flick: 50 ms with the hand at rest
    T::WindowVerdict b;
    b.Step(11, Det(T::Verdict::Pending, 6), dt);
    b.Step(0, Det(T::Verdict::Pending, 6), dt);
    EXPECT_TRUE(b.verdict == T::Verdict::Swing);
    EXPECT_STREQ(b.why, "the window closed");
}

TEST(VisceralThrustWindow, DetectorAndWindowOnARealSwing) {
    // The forehand of ForehandSwingDoesNot through the window verdict: the window opens 30 ms into
    // the stroke (the onset) and is a swing within 90 ms of the stroke's start.
    const Path p = Arc(-75.0f, 5.0f, 45.0f, -10.0f, 6.0f, 0.2);
    T::Detector d;
    T::WindowVerdict wv;
    const double h = 1.0 / Hz;
    double decided = -1.0;
    for (double t = 0.0; t <= 1.0; t += h) {
        const Sample a = p(t - 0.004), b = p(t + 0.004), s = p(t);
        T::Input in{};
        in.t = t;
        in.active = true;
        in.valid = true;
        in.v = (b.hand - a.hand) * (1.0f / 0.008f);
        in.head_ok = true;
        in.blade = s.blade;
        const T::Output o = d.Update(in, T::Config{});
        wv.Step(t >= 0.23 && t < 0.5 ? 12u : 0u, o, static_cast<float>(h));
        if (decided < 0.0 && wv.window == 12u && wv.verdict != T::Verdict::Pending) {
            decided = t;
            EXPECT_TRUE(wv.verdict == T::Verdict::Swing);
        }
    }
    EXPECT_GT(decided, 0.0);
    EXPECT_LT(decided - 0.2, 0.09);
}

// ---- The heartbeat --------------------------------------------------------------------------------

TEST(VisceralThrustCue, OnceAtTheStart) {
    T::Cue c;
    const float dt = 0.1f;
    EXPECT_FALSE(c.Step(1, 0, false, dt));
    EXPECT_TRUE(c.Step(1, 1, true, dt));
    int beats = 0;
    for (int i = 0; i < 40; ++i) {
        beats += c.Step(1, 1, true, dt) ? 1 : 0;
    }
    EXPECT_EQ(beats, 0);
    EXPECT_FALSE(c.Step(1, 1, false, dt));
    EXPECT_TRUE(c.Step(1, 2, true, dt)); // the next parry
}

TEST(VisceralThrustCue, RepeatsAndOff) {
    T::Cue c;
    const float dt = 0.05f;
    int beats = c.Step(2, 1, true, dt) ? 1 : 0;
    for (int i = 0; i < 100; ++i) { // 5 s open
        beats += c.Step(2, 1, true, dt) ? 1 : 0;
    }
    EXPECT_EQ(beats, 3);
    T::Cue off;
    EXPECT_FALSE(off.Step(0, 1, true, dt));
    // A serial seen while the cue was off does not beat later.
    EXPECT_FALSE(off.Step(1, 1, true, dt));
}
