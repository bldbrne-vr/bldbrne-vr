// SPDX-License-Identifier: GPL-2.0-or-later

// Synthetic swings for the motion-weapon classifier (core/vr/swing_detector.h). Hand and blade
// follow min-jerk rotations about a shoulder or torso pivot and are sampled like the OpenXR
// frame loop (72/90/120 Hz, dropped frames, pose prediction, orientation noise). Expected
// results come from the Python reference model the thresholds were tuned with (cases after
// T26 from this implementation): 90 Hz unless noted, times in ms after motion onset (t = 0.10 s).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/swing_detector.h"
#ifdef PI_MELEE_DLL_TEST
#include "melee_dll_test_driver.h"
using SwingTestClassifier = DllTestClassifier;
#else
using SwingTestClassifier = VR::Melee::Classifier;
#endif


namespace {

namespace M = VR::Melee;

constexpr double Pi = 3.14159265358979323846;
constexpr double Rad = Pi / 180.0;

// ---- Double-precision geometry for the trajectories --------------------------------------

struct D3 {
    double x{}, y{}, z{};
};

D3 operator+(D3 a, D3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
D3 operator-(D3 a, D3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
D3 operator*(D3 a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
double Dot(D3 a, D3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
D3 Cross(D3 a, D3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Len(D3 a) {
    return std::sqrt(Dot(a, a));
}
D3 Unit(D3 a) {
    const double l = Len(a);
    return l > 1e-12 ? a * (1.0 / l) : D3{};
}
double AngleBetween(D3 a, D3 b) {
    return std::atan2(Len(Cross(a, b)), Dot(a, b));
}
// Rotates v about axis k by ang radians (Rodrigues).
D3 Rod(D3 v, D3 k, double ang) {
    k = Unit(k);
    const double c = std::cos(ang), s = std::sin(ang);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0 - c));
}
// Rotation about +Y (positive = toward the left).
D3 Yaw(D3 v, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
M::V3 F(D3 a) {
    return {static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)};
}
// Shortest-arc grip orientation that points grip -Z (the hilt) along the blade.
M::Q4 GripFor(D3 blade) {
    const D3 hilt{0.0, 0.0, -1.0};
    const double d = Dot(hilt, blade);
    if (d < -0.999999) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    const D3 c = Cross(hilt, blade);
    const double w = 1.0 + d;
    const double n = std::sqrt(Dot(c, c) + w * w);
    return {static_cast<float>(c.x / n), static_cast<float>(c.y / n), static_cast<float>(c.z / n),
            static_cast<float>(w / n)};
}

// Min-jerk profile and its derivative.
double S(double tau) {
    tau = std::clamp(tau, 0.0, 1.0);
    return 10.0 * std::pow(tau, 3) - 15.0 * std::pow(tau, 4) + 6.0 * std::pow(tau, 5);
}
double DS(double tau) {
    if (tau <= 0.0 || tau >= 1.0) {
        return 0.0;
    }
    return 30.0 * std::pow(tau, 2) - 60.0 * std::pow(tau, 3) + 30.0 * std::pow(tau, 4);
}

// Directions: El = sagittal elevation (0 forward, 90 up, 180 back), Az = azimuth (+ right),
// Dg = diagonal plane from forward toward up-right.
D3 El(double e) {
    return {0.0, std::sin(e * Rad), -std::cos(e * Rad)};
}
D3 Az(double a) {
    return {std::sin(a * Rad), 0.0, -std::cos(a * Rad)};
}
D3 Dg(double phi) {
    const D3 u1 = Unit({1.0, 1.0, 0.0});
    const D3 u2{0.0, 0.0, -1.0};
    return Unit(u2 * std::cos(phi * Rad) + u1 * std::sin(phi * Rad));
}
D3 At(D3 c, double r, D3 d) {
    return c + d * r;
}

// Pivots relative to the eyes, and rotation axes.
const D3 SH{0.18, -0.25, 0.05};
const D3 BODY{0.05, -0.30, 0.05};
const D3 DOWN{-1.0, 0.0, 0.0};
const D3 UP{1.0, 0.0, 0.0};
const D3 LEFT{0.0, 1.0, 0.0};
const D3 RIGHT{0.0, -1.0, 0.0};
const D3 KD = Unit({-1.0, 1.0, 0.0});

// ---- Trajectories ---------------------------------------------------------------------------

struct Sample {
    D3 b; // blade direction
    D3 p; // hand position relative to the eyes
    D3 v; // hand velocity
    D3 w; // angular velocity (pose prediction only)
};
using Traj = std::function<Sample(double)>;

// Rigid rotation of hand and blade about pivot c and axis k by deg degrees over T seconds from
// t0, min-jerk, optionally time-warped (tau -> tau^g).
struct Rot {
    D3 c, k;
    double th, T, t0;
    D3 h0, b0;
    double g;

    Rot(D3 c_, D3 k_, double deg, double T_, double t0_, D3 h0_, D3 b0_, double g_ = 1.0)
        : c{c_}, k{Unit(k_)}, th{deg * Rad}, T{T_}, t0{t0_}, h0{h0_}, b0{Unit(b0_)}, g{g_} {}

    double Angle(double t) const {
        const double tau = std::clamp((t - t0) / T, 0.0, 1.0);
        return th * S(std::pow(tau, g));
    }
    double Rate(double t) const {
        const double tau = (t - t0) / T;
        if (tau <= 0.0 || tau >= 1.0) {
            return 0.0;
        }
        return th * DS(std::pow(tau, g)) * g * std::pow(tau, g - 1.0) / T;
    }
    D3 EndHand() const {
        return c + Rod(h0 - c, k, th);
    }
    D3 EndBlade() const {
        return Rod(b0, k, th);
    }
    Sample operator()(double t) const {
        const double a = Angle(t);
        const double rate = Rate(t);
        const D3 r = Rod(h0 - c, k, a);
        return {Rod(b0, k, a), c + r, Cross(k, r) * rate, k * rate};
    }
};

// Straight hand move with the blade slerped between two directions.
struct Move {
    D3 ha, hb, ba, bb, k;
    double ang, T, t0;

    Move(D3 ha_, D3 hb_, D3 ba_, D3 bb_, double T_, double t0_)
        : ha{ha_}, hb{hb_}, ba{Unit(ba_)}, bb{Unit(bb_)}, k{Unit(Cross(ba, bb))},
          ang{AngleBetween(ba, bb)}, T{T_}, t0{t0_} {}

    Sample operator()(double t) const {
        const double tau = (t - t0) / T;
        const double s = S(tau);
        const double ds = DS(tau) / T;
        return {Rod(ba, k, ang * s), ha + (hb - ha) * s, (hb - ha) * ds, k * (ang * ds)};
    }
};

// Oscillation about a pivot: angle = A sin(2 pi f (t - t0)) for dur seconds.
struct Osc {
    D3 c, k;
    double A, f;
    D3 h0, b0;
    double t0, dur;

    Osc(D3 c_, D3 k_, double deg, double f_, D3 h0_, D3 b0_, double t0_, double dur_)
        : c{c_}, k{Unit(k_)}, A{deg * Rad}, f{f_}, h0{h0_}, b0{Unit(b0_)}, t0{t0_}, dur{dur_} {}

    Sample operator()(double t) const {
        const double tt = std::clamp(t - t0, 0.0, dur);
        const double a = A * std::sin(2.0 * Pi * f * tt);
        const double rate =
            (t - t0 > 0.0 && t - t0 < dur) ? A * 2.0 * Pi * f * std::cos(2.0 * Pi * f * tt) : 0.0;
        const D3 r = Rod(h0 - c, k, a);
        return {Rod(b0, k, a), c + r, Cross(k, r) * rate, k * rate};
    }
};

// Piecewise trajectory: each segment is used up to (and including) its end time.
Traj Seq(std::vector<std::pair<Traj, double>> segments) {
    return [segments](double t) {
        for (const auto& [segment, end] : segments) {
            if (t <= end) {
                return segment(t);
            }
        }
        return segments.back().first(t);
    };
}

// Hand and blade turn with the head (whole body turn about the eyes) by yaw(t).
Traj Yawed(Traj f, std::function<double(double)> yaw, std::function<double(double)> yaw_rate) {
    return [=](double t) {
        const Sample s = f(t);
        const double a = yaw(t);
        const D3 p = Yaw(s.p, a);
        return Sample{Yaw(s.b, a), p, Yaw(s.v, a) + Cross({0.0, 1.0, 0.0}, p) * yaw_rate(t), s.w};
    };
}

// Weapon held still in guard.
Sample Guard(double) {
    return {El(40), {0.25, -0.35, -0.35}, {}, {}};
}

Sample Stir(double t) {
    // Blade circling a 30 degree cone around El(30) at 2 Hz, hand still.
    const double ph = 2.0 * Pi * 2.0 * std::max(t - 0.1, 0.0);
    const D3 c0 = El(30);
    const D3 u = Unit(Cross(c0, {1.0, 0.0, 0.0}));
    const D3 w = Cross(c0, u);
    const double cone = 30.0 * Rad;
    const D3 b = Unit(c0 * std::cos(cone) + u * (std::sin(cone) * std::cos(ph)) +
                      w * (std::sin(cone) * std::sin(ph)));
    const D3 hand{0.25, -0.35, -0.35};
    return {b, hand + b * 0.08, {}, {}};
}

// The trajectories of the spec (T01..T26).
Rot Flick() {
    const D3 h0{0.25, -0.35, -0.35};
    return Rot(h0 + D3{0.0, 0.0, 0.06}, DOWN, 100, 0.12, 0.10, h0, El(45));
}
Rot Forehand(double t0 = 0.10) {
    return Rot(BODY, LEFT, 100, 0.22, t0, {0.32, -0.30, -0.20}, Az(70));
}
Rot Backhand() {
    return Rot(BODY, RIGHT, 110, 0.24, 0.10, {-0.10, -0.30, -0.30}, Az(-60));
}
Move Stab() {
    return Move({0.20, -0.35, -0.15}, {0.20, -0.40, -0.55}, El(-10), El(-10), 0.20, 0.10);
}
Rot Overhead(double T = 0.35, double deg = 170, double t0 = 0.10, double r = 0.5, double he = 110,
             double be = 130, double g = 1.0) {
    return Rot(SH, DOWN, deg, T, t0, At(SH, r, El(he)), El(be), g);
}
Rot Sweep(double t0 = 0.10) {
    return Rot(BODY, LEFT, 190, 0.40, t0, At(BODY, 0.55, Az(120)), Az(135));
}
Rot Diagonal(double t0 = 0.10) {
    return Rot(SH, KD, 180, 0.38, t0, At(SH, 0.5, Dg(100)), Dg(120));
}
Rot RaiseOnly() {
    return Rot(SH, UP, 130, 0.28, 0.10, At(SH, 0.45, El(-20)), El(45));
}
Rot SideWindup() {
    return Rot(BODY, RIGHT, 120, 0.28, 0.10, At(BODY, 0.50, Az(10)), Az(20));
}
Osc WristWaggle() {
    const D3 h{0.25, -0.35, -0.35};
    return Osc(h + D3{0.0, 0.0, 0.10}, LEFT, 30, 3.0, h, Az(0), 0.10, 2.0);
}
Osc ArmWaggle() {
    return Osc(BODY, LEFT, 50, 2.5, At(BODY, 0.45, Az(0)), Az(0), 0.10, 2.0);
}
Rot SeatedHeavy() {
    return Rot(SH, DOWN, 125, 0.30, 0.10, At(SH, 0.30, El(100)), El(115));
}
Rot DownChop() {
    return Rot(SH, DOWN, 70, 0.22, 0.10, At(SH, 0.5, El(0)), El(0));
}
Rot CockedShortChop() {
    return Overhead(0.22, 100, 0.10, 0.5, 110, 125);
}
Rot RaisedGuardChop() {
    return Rot(SH, DOWN, 105, 0.25, 0.10, {0.18, -0.12, -0.25}, El(85));
}
Traj TwoLights() {
    const Rot f = Forehand(0.10);
    const Rot b(BODY, RIGHT, 100, 0.22, 0.50, f.EndHand(), f.EndBlade());
    return Seq({{f, 0.49}, {b, 9.0}});
}
Traj LightThenBig() {
    const Rot f = Forehand(0.10);
    const Rot o = Overhead(0.35, 170, 0.74);
    const Move m(f.EndHand(), o.h0, f.EndBlade(), o.b0, 0.30, 0.36);
    return Seq({{f, 0.355}, {m, 0.735}, {o, 9.0}});
}
Traj WindupIntoSweep() {
    const Rot s = Sweep(0.38);
    const Move m(At(BODY, 0.5, Az(10)), s.h0, Az(20), s.b0, 0.28, 0.10);
    return Seq({{m, 0.38}, {s, 9.0}});
}
Traj RaiseIntoOverhead(double pause) {
    const Rot o = Overhead(0.35, 170, 0.40 + pause);
    const Move m(At(SH, 0.45, El(-20)), o.h0, El(45), o.b0, 0.30, 0.10);
    return Seq({{m, 0.40 + pause - 1e-6}, {o, 9.0}});
}
// Body turn by deg degrees (+ = left) in T seconds from t = 0.10.
double TurnYaw(double t, double deg = 45.0, double T = 0.35) {
    return deg * Rad * S((t - 0.10) / T);
}
double TurnYawRate(double t, double deg = 45.0, double T = 0.35) {
    return deg * Rad * DS((t - 0.10) / T) / T;
}
Traj BodyTurnSweep() {
    const Rot base(BODY, LEFT, 125, 0.35, 0.10, At(BODY, 0.55, Az(110)), Az(125));
    return Yawed(
        base, [](double t) { return TurnYaw(t); }, [](double t) { return TurnYawRate(t); });
}
// The weapon held in guard through a quick body turn.
Traj TurnInGuard(double deg, double T) {
    return Yawed(
        Guard, [=](double t) { return TurnYaw(t, deg, T); },
        [=](double t) { return TurnYawRate(t, deg, T); });
}
Rot LateWhip() {
    return Overhead(0.30, 130, 0.10, 0.5, 110, 125, 1.6);
}
Rot EarlyPeak() {
    return Overhead(0.35, 160, 0.10, 0.5, 110, 130, 0.65);
}
Move PullToFace() {
    return Move({0.25, -0.35, -0.55}, {0.10, -0.10, -0.10}, El(10), El(80), 0.20, 0.10);
}
Rot BigFromGuard() {
    return Rot(SH, DOWN, 135, 0.30, 0.10, At(SH, 0.45, El(10)), El(45));
}
Rot WideFromRight() {
    return Rot(BODY, LEFT, 160, 0.35, 0.10, At(BODY, 0.55, Az(90)), Az(95));
}
Traj DiagonalWindup() {
    const Rot d = Diagonal(0.5);
    return Move(At(SH, 0.45, El(-20)), d.h0, El(45), d.b0, 0.28, 0.10);
}
Traj DiagonalWindupIntoSwing() {
    const Rot d = Diagonal(0.38);
    const Move m(At(SH, 0.45, El(-20)), d.h0, El(45), d.b0, 0.28, 0.10);
    return Seq({{m, 0.38}, {d, 9.0}});
}
Rot WideFromLeft() {
    return Rot(BODY, RIGHT, 160, 0.35, 0.10, At(BODY, 0.55, Az(-90)), Az(-95));
}
Move PullBack() {
    return Move({0.20, -0.35, -0.55}, {0.20, -0.55, -0.10}, El(0), El(-60), 0.20, 0.10);
}

// ---- Overhead swings (QA 2026-10-07) ------------------------------------------------------------

// A path through keys (time, hand, blade direction): Catmull-Rom with non-uniform times, still
// before the first key and after the last; velocities by central differences.
struct PathKey {
    double t;
    D3 p, b;
};

D3 KeyedAt(const std::vector<PathKey>& k, double t, bool blade) {
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
    const double t0 = k[i].t, t1 = k[i + 1].t, s = (t - t0) / (t1 - t0);
    const auto tangent = [&](std::size_t j) {
        return j == 0 || j + 1 >= n ? D3{}
                                    : (val(j + 1) - val(j - 1)) * (1.0 / (k[j + 1].t - k[j - 1].t));
    };
    const D3 m0 = tangent(i) * (t1 - t0), m1 = tangent(i + 1) * (t1 - t0);
    const double s2 = s * s, s3 = s2 * s;
    return val(i) * (2 * s3 - 3 * s2 + 1) + m0 * (s3 - 2 * s2 + s) +
           val(i + 1) * (-2 * s3 + 3 * s2) + m1 * (s3 - s2);
}

Traj Keyed(std::vector<PathKey> keys) {
    return [keys](double t) {
        constexpr double h = 1e-4;
        const D3 b0 = Unit(KeyedAt(keys, t - h, true)), b1 = Unit(KeyedAt(keys, t + h, true));
        return Sample{Unit(KeyedAt(keys, t, true)), KeyedAt(keys, t, false),
                      (KeyedAt(keys, t + h, false) - KeyedAt(keys, t - h, false)) * (0.5 / h),
                      Cross(b0, b1) * (0.5 / h)};
    };
}

// Shifts a key list's times so it starts at `t0` and scales them by `k` (the speed).
std::vector<PathKey> Timed(std::vector<PathKey> keys, double t0, double k) {
    const double first = keys.front().t;
    for (PathKey& key : keys) {
        key.t = t0 + (key.t - first) * k;
    }
    return keys;
}

const D3 GuardHand{0.22, -0.35, -0.35};

// Wound up from guard out to the right and up, behind the right shoulder, over the top and down
// through the front, in one motion: the blade's axis turns gradually and never reverses.
std::vector<PathKey> LoopOverShoulderKeys() {
    return {{0.00, GuardHand, El(40)},
            {0.20, {0.38, -0.05, -0.15}, Unit({0.9, 0.6, -0.2})},
            {0.36, {0.28, 0.18, 0.05}, Unit({0.4, 0.5, 0.8})},
            {0.46, {0.16, 0.26, 0.0}, El(125)},
            {0.58, {0.15, 0.0, -0.42}, El(30)},
            {0.68, {0.12, -0.45, -0.38}, El(-40)}};
}

// Wound up from guard over the right shoulder and behind the head (the blade back to the left),
// then over the top and down: the strike's blade first rises from behind the head.
std::vector<PathKey> LoopBehindHeadKeys() {
    return {{0.00, GuardHand, El(40)},
            {0.15, {0.25, 0.05, -0.05}, Unit({0.3, 0.9, 0.2})},
            {0.28, {0.10, 0.22, 0.08}, Unit({-0.5, 0.3, 0.8})},
            {0.35, {0.15, 0.25, 0.0}, El(140)},
            {0.475, {0.15, -0.075, -0.32}, El(80)},
            {0.60, {0.15, -0.40, -0.40}, El(20)}};
}

// The session's window #17 (stroke #87: cocked back 60 deg in 250 ms at 1.2 m/s, then on round
// behind the head and over into the chop, 235 deg in 867 ms, the blade 19.7 rad/s, the hand
// 3.5 m/s at the window): a slow wind-up flowing into a fast chop as one stroke.
std::vector<PathKey> SlowCockIntoChopKeys() {
    return {{0.00, GuardHand, El(40)},
            {0.25, {0.26, -0.15, -0.20}, El(100)},
            {0.50, {0.24, 0.10, 0.02}, Unit({0.45, 0.4, 0.8})},
            {0.68, {0.14, 0.24, 0.04}, El(135)},
            {0.80, {0.16, -0.02, -0.40}, El(40)},
            {0.90, {0.13, -0.42, -0.38}, El(-35)}};
}

// The loop over the shoulder held there (no strike) / brought back down to guard slowly.
std::vector<PathKey> LoopOnlyKeys() {
    return {{0.00, GuardHand, El(40)},
            {0.20, {0.38, -0.05, -0.15}, Unit({0.9, 0.6, -0.2})},
            {0.36, {0.28, 0.18, 0.05}, Unit({0.4, 0.5, 0.8})},
            {0.48, {0.16, 0.26, 0.04}, El(135)}};
}
std::vector<PathKey> LoopThenLowerKeys() {
    std::vector<PathKey> k = LoopOnlyKeys();
    k.push_back({0.80, {0.16, 0.26, 0.04}, El(135)});
    k.push_back({1.60, GuardHand, El(40)});
    return k;
}
// Fast heavy: the arm sweeps 120 deg about the shoulder in 250 ms and the wrist snaps another
// 60 deg at the end (peak blade rate above GlitchRate).
Traj WristSnapHeavy(double snap) {
    const Rot arm = Overhead(0.25, 120, 0.10);
    return [=](double t) {
        Sample s = arm(t);
        const double tau = (t - (0.33 - snap)) / snap;
        s.b = Rod(s.b, DOWN, 60.0 * Rad * S(tau));
        s.w = s.w + Unit(DOWN) * (60.0 * Rad * DS(tau) / snap);
        return s;
    };
}
// A 120 deg wound-up chop (light) whose grip orientation jumps by deg for one frame at its
// peak rate.
Traj GlitchedChop(double deg, int hz, D3 axis) {
    const Rot chop = Overhead(0.30, 120, 0.10, 0.5, 110, 130);
    return [=](double t) {
        Sample s = chop(t);
        if (t >= 0.25 && t < 0.25 + 1.0 / hz - 1e-6) {
            s.b = Rod(s.b, axis, deg * Rad);
        }
        return s;
    };
}

// ---- Harness ----------------------------------------------------------------------------------

struct Options {
    int hz{90};
    bool seated{false};
    bool arc{true};
    float heavy_arc_deg{140.0f};
    double pred{0.0};   // constant-velocity pose prediction (s)
    int drop_every{0};  // drop every n-th frame
    double drop_p{0.0}; // drop frames at random with this probability
    double noise{0.0};  // orientation noise (deg)
    unsigned seed{1};
    bool turn{false}; // head yawed by TurnYaw(t, turn_deg, turn_s)
    double turn_deg{45.0};
    double turn_s{0.35};
    double pos_lost[2]{-1.0, -1.0};
    double ori_lost[2]{-1.0, -1.0};
    double head_lost[2]{-1.0, -1.0};
    double not_immersive[2]{-1.0, -1.0};
    double release_at{-1.0}; // grip released (weapon sheathed) from this time on
    double trigger_at{-1.0}; // right trigger pressed (heavy route) at this time
    bool heavy_busy{false};  // the trigger heavy machine is busy (charging / held)
    bool clutchless{false};  // no grip clutch (right_grip=transform): Config::clutchless
    bool overhead{true};     // Config::windup_turn / toward_front / rejudge (false: all off)
    // Head translation (re-localization slides): offset and velocity.
    std::function<D3(double)> head_offset;
    std::function<D3(double)> head_velocity;
};

struct Event {
    double ms; // after motion onset (t = 0.10 s)
    M::Kind kind;
    std::string reason;
    float arc; // theta_eff, degrees
    float a0;
    bool cocked;
    bool consume;
};

struct Result {
    std::vector<Event> events;
    std::vector<double> clicks; // ready clicks, ms after onset
};

bool Within(double t, const double window[2]) {
    return t >= window[0] && t < window[1];
}

void Record(Result& result, double ms, M::Kind kind, const char* reason, const M::Output& out) {
    result.events.push_back({ms, kind, reason, out.stats.theta_eff, out.stats.alpha0,
                             out.stats.cocked, out.consume_trigger});
}

Result RunSwing(const Traj& traj, double dur, const Options& o = {}) {
    const D3 eye{0.0, o.seated ? 1.20 : 1.60, 0.0};
    M::Config cfg = M::Config::Make(o.arc, true, o.heavy_arc_deg, 100.0f, o.seated, 0.0f);
    cfg.clutchless = o.clutchless;
    if (!o.overhead) {
        cfg.windup_turn = 0.0f;
        cfg.toward_front = 0.0f;
        cfg.rejudge = false;
    }
    SwingTestClassifier classifier;
    std::mt19937 rng{o.seed};
    std::normal_distribution<double> gauss{0.0, 1.0};
    std::uniform_real_distribution<double> uniform{0.0, 1.0};
    Result result;
    const double dt = 1.0 / o.hz;
    double t = 0.0;
    int k = 0;
    bool trigger_pressed = false;
    bool was_active = false;
    while (t < dur) {
        ++k;
        t = k * dt;
        if (o.drop_every > 0 && k % o.drop_every == 0) {
            continue;
        }
        if (o.drop_p > 0.0 && uniform(rng) < o.drop_p) {
            continue;
        }
        Sample s = traj(t + o.pred);
        if (o.pred > 0.0) {
            const Sample now = traj(t);
            const double w = Len(now.w);
            s = {w > 1e-9 ? Rod(now.b, now.w, w * o.pred) : now.b, now.p + now.v * o.pred, now.v,
                 now.w};
        }
        if (o.noise > 0.0) {
            const D3 axis = Unit({gauss(rng), gauss(rng), gauss(rng)});
            s.b = Rod(s.b, axis, gauss(rng) * o.noise * Rad);
            s.p = s.p + D3{gauss(rng), gauss(rng), gauss(rng)} * 0.002;
            s.v = s.v + D3{gauss(rng), gauss(rng), gauss(rng)} * 0.03;
        }
        const double yaw = o.turn ? TurnYaw(t, o.turn_deg, o.turn_s) : 0.0;
        const bool edge = o.trigger_at >= 0.0 && t >= o.trigger_at && !trigger_pressed;
        trigger_pressed = trigger_pressed || edge;
        const D3 head = o.head_offset ? eye + o.head_offset(t) : eye;
        const D3 head_v = o.head_velocity ? o.head_velocity(t) : D3{};

        M::Input in{};
        in.t = t;
        in.active = !(o.release_at >= 0.0 && t >= o.release_at);
        in.immersive = !Within(t, o.not_immersive);
        in.heavy_idle = !o.heavy_busy && !trigger_pressed;
        in.trigger_heavy_edge = edge;
        in.ori_valid = !Within(t, o.ori_lost);
        in.q = GripFor(s.b);
        in.pos_valid = !Within(t, o.pos_lost);
        in.p = F(eye + s.p);
        in.v = F(s.v);
        in.head_ok = !Within(t, o.head_lost);
        in.head_p = F(head);
        in.head_v = in.head_ok ? F(head_v) : M::V3{};
        in.head_fwd = {static_cast<float>(-std::sin(yaw)), 0.0f,
                       static_cast<float>(-std::cos(yaw))};
        if (in.active && !was_active) {
            classifier.OnDraw(); // as the presenter does when the grip is squeezed
        }
        was_active = in.active;
        const M::Output out = classifier.Update(in, cfg);
        const double ms = (t - 0.10) * 1000.0;
        if (out.ready_click) {
            result.clicks.push_back(ms);
        }
        if (out.gate != M::Kind::None) {
            Record(result, ms, out.gate, out.gate_reason, out);
        }
        if (out.kind != M::Kind::None) {
            Record(result, ms, out.kind, out.reason, out);
        }
    }
    return result;
}

const char* KindName(M::Kind kind) {
    switch (kind) {
    case M::Kind::Light:
        return "R1";
    case M::Kind::Heavy:
        return "R2";
    case M::Kind::Commit:
        return "Commit";
    case M::Kind::Ignored:
        return "Ignored";
    case M::Kind::Rejected:
        return "Rejected";
    default:
        return "None";
    }
}

// Same format as the Python reference model.
std::string Format(const Result& r) {
    std::string text;
    for (const Event& e : r.events) {
        char line[160];
        std::snprintf(line, sizeof(line), "%s%s(%s)@%.0fms arc%.0f a0 %.0f%s",
                      text.empty() ? "" : "; ", KindName(e.kind), e.reason.c_str(), e.ms, e.arc,
                      e.a0, e.cocked ? " C" : "");
        text += line;
    }
    return text.empty() ? "nothing" : text;
}

// Attacks only (R1 / R2), in order.
std::string Attacks(const Result& r) {
    std::string text;
    for (const Event& e : r.events) {
        if (e.kind == M::Kind::Light || e.kind == M::Kind::Heavy) {
            text += text.empty() ? "" : ",";
            text += KindName(e.kind);
        }
    }
    return text;
}

struct Expected {
    M::Kind kind;
    const char* reason;
    double ms;
};

// Kinds and reasons must match exactly, times within one 90 Hz frame.
void ExpectEvents(const char* name, const Result& r, const std::vector<Expected>& want) {
    EXPECT_EQ(r.events.size(), want.size()) << name << ": " << Format(r);
    for (size_t i = 0; i < std::min(r.events.size(), want.size()); ++i) {
        EXPECT_EQ(r.events[i].kind, want[i].kind) << name << " #" << i << ": " << Format(r);
        EXPECT_EQ(r.events[i].reason, std::string(want[i].reason))
            << name << " #" << i << ": " << Format(r);
        EXPECT_NEAR(r.events[i].ms, want[i].ms, 12.0) << name << " #" << i << ": " << Format(r);
    }
}

constexpr M::Kind Light = M::Kind::Light;
constexpr M::Kind Heavy = M::Kind::Heavy;
constexpr M::Kind Ignored = M::Kind::Ignored;
constexpr M::Kind Rejected = M::Kind::Rejected;
constexpr M::Kind Commit = M::Kind::Commit;

struct Case {
    const char* name;
    Traj traj;
    double dur;
    Options options;
    std::vector<Expected> events; // 90 Hz reference
    const char* attacks;          // under every sampling condition
};

std::vector<Case> Cases() {
    Options seated{};
    seated.seated = true;
    Options pos_lost{};
    pos_lost.pos_lost[0] = 0.22;
    pos_lost.pos_lost[1] = 0.32;
    Options ori_lost{};
    ori_lost.ori_lost[0] = 0.20;
    ori_lost.ori_lost[1] = 0.35;
    Options turn{};
    turn.turn = true;
    return {
        {"T01 small wrist flick", Flick(), 0.6, {}, {}, ""},
        {"T02 normal forehand", Forehand(), 0.6, {}, {{Light, "guard", 78}}, "R1"},
        {"T03 normal backhand", Backhand(), 0.6, {}, {{Light, "guard", 89}}, "R1"},
        {"T04 normal stab", Stab(), 0.6, {}, {{Light, "thrust", 100}}, "R1"},
        {"T05 big overhead", Overhead(), 0.8, {}, {{Heavy, "early", 211}}, "R2"},
        {"T06 big horizontal sweep", Sweep(), 0.8, {}, {{Heavy, "early", 222}}, "R2"},
        {"T07 big diagonal", Diagonal(), 0.8, {}, {{Heavy, "early", 222}}, "R2"},
        {"T08 slow big arc", Overhead(2.0), 2.5, {}, {}, ""},
        {"T09 raise only", RaiseOnly(), 0.9, {}, {{Ignored, "raising", 89}}, ""},
        {"T09b sideways wind-up only", SideWindup(), 0.9, {}, {{Ignored, "cocking", 89}}, ""},
        {"T10 wrist waggle", WristWaggle(), 2.3, {}, {}, ""},
        {"T11 seated deliberate heavy", SeatedHeavy(), 0.7, seated, {{Heavy, "arc", 256}}, "R2"},
        {"T11 same, standing", SeatedHeavy(), 0.7, {}, {{Light, "stop", 244}}, "R1"},
        {"T12 position dropout", Overhead(), 0.8, pos_lost, {{Heavy, "early", 211}}, "R2"},
        {"T12b orientation dropout", Overhead(), 0.8, ori_lost, {{Light, "tracking", 189}}, "R1"},
        {"T13 two quick lights",
         TwoLights(),
         1.0,
         {},
         {{Light, "guard", 78}, {Light, "guard", 478}},
         "R1,R1"},
        {"T14 light, then a big arc",
         LightThenBig(),
         1.4,
         {},
         {{Light, "guard", 78}, {Ignored, "raising", 367}, {Heavy, "arc", 889}},
         "R1,R2"},
        {"T15 sideways wind-up into the sweep",
         WindupIntoSweep(),
         1.1,
         {},
         {{Ignored, "cocking", 100}, {Heavy, "early", 511}},
         "R2"},
        {"T15b raise, pause, overhead",
         RaiseIntoOverhead(0.10),
         1.2,
         {},
         {{Ignored, "raising", 100}, {Heavy, "early", 611}},
         "R2"},
        {"T15c raise into overhead",
         RaiseIntoOverhead(0.0),
         1.2,
         {},
         {{Ignored, "raising", 100}, {Heavy, "early", 511}},
         "R2"},
        {"T16 down-chop from level", DownChop(), 0.6, {}, {{Light, "guard", 89}}, "R1"},
        {"T17 wound-up short chop", CockedShortChop(), 0.6, {}, {{Light, "stop", 167}}, "R1"},
        {"T18 raised-guard chop", RaisedGuardChop(), 0.6, {}, {{Light, "guard", 100}}, "R1"},
        {"T19 sweep during a body turn", BodyTurnSweep(), 0.8, turn, {{Light, "stop", 289}}, "R1"},
        {"T20 late-peak whip", LateWhip(), 0.7, {}, {{Light, "stop", 289}}, "R1"},
        {"T21 early-peak heavy", EarlyPeak(), 0.7, {}, {{Heavy, "arc", 222}}, "R2"},
        {"T23 pull hand to face", PullToFace(), 0.6, {}, {{Ignored, "raising", 78}}, ""},
        {"T24 exaggerated swing from guard", BigFromGuard(), 0.7, {}, {{Light, "guard", 89}}, "R1"},
        {"T25 wide sweep, arm out right", WideFromRight(), 0.8, {}, {{Heavy, "arc", 267}}, "R2"},
        {"T26 stir", Stir, 2.2, {}, {}, ""},
        {"X1 diagonal wind-up only", DiagonalWindup(), 0.9, {}, {{Ignored, "cocking", 100}}, ""},
        {"X2 diagonal wind-up into the swing",
         DiagonalWindupIntoSwing(),
         1.1,
         {},
         {{Ignored, "cocking", 100}, {Heavy, "early", 500}},
         "R2"},
        {"X3 wound-up wide horizontal light",
         Rot(BODY, LEFT, 100, 0.22, 0.10, At(BODY, 0.45, Az(90)), Az(95)),
         0.6,
         {},
         {{Light, "stop", 167}},
         "R1"},
        {"X4 seated overhead 150",
         Overhead(0.30, 150, 0.10, 0.40, 100, 120),
         0.8,
         seated,
         {{Heavy, "early", 178}},
         "R2"},
        {"X5 seated light chop from guard",
         Rot(SH, DOWN, 80, 0.20, 0.10, At(SH, 0.35, El(10)), El(45)),
         0.6,
         seated,
         {{Light, "guard", 89}},
         "R1"},
        {"X6 seated horizontal light",
         Rot(BODY, LEFT, 90, 0.22, 0.10, At(BODY, 0.32, Az(75)), Az(80)),
         0.6,
         seated,
         {{Light, "stop", 167}},
         "R1"},
        {"T25b wide backhand, arm out left", WideFromLeft(), 0.8, {}, {{Heavy, "arc", 267}}, "R2"},
        {"T16b fast pull-back toward the body",
         PullBack(),
         0.6,
         {},
         {{Ignored, "toward the body", 89}},
         ""},
    };
}

TEST(SwingArcTest, ReferenceTrajectoriesAt90Hz) {
    for (const Case& c : Cases()) {
        ExpectEvents(c.name, RunSwing(c.traj, c.dur, c.options), c.events);
    }
}

TEST(SwingArcTest, SameClassesUnderEverySamplingCondition) {
    struct Condition {
        const char* name;
        int hz;
        int drop_every;
        double drop_p;
        double pred;
        double noise;
    };
    const Condition conditions[] = {{"72 Hz", 72, 0, 0.0, 0.0, 0.0},
                                    {"120 Hz", 120, 0, 0.0, 0.0, 0.0},
                                    {"90 Hz, every 3rd frame dropped", 90, 3, 0.0, 0.0, 0.0},
                                    {"90 Hz, 10% random drops", 90, 0, 0.10, 0.0, 0.0},
                                    {"90 Hz, +35 ms prediction", 90, 0, 0.0, 0.035, 0.0},
                                    {"90 Hz, 0.3 deg noise", 90, 0, 0.0, 0.0, 0.3}};
    for (const Condition& condition : conditions) {
        for (const Case& c : Cases()) {
            Options o = c.options;
            o.hz = condition.hz;
            o.drop_every = condition.drop_every;
            o.drop_p = condition.drop_p;
            if (condition.drop_p > 0.0) {
                o.seed = 7;
            }
            o.pred = condition.pred;
            o.noise = condition.noise;
            const Result r = RunSwing(c.traj, c.dur, o);
            EXPECT_EQ(Attacks(r), std::string(c.attacks))
                << c.name << " at " << condition.name << ": " << Format(r);
        }
    }
}

TEST(SwingArcTest, ArmWaggleIsNeverHeavy) {
    // T10b: the first outward half is a wind-up; then one light per 0.4 s cycle, with "too
    // soon" rejections in between.
    const Result r = RunSwing(ArmWaggle(), 2.3);
    ExpectEvents("T10b arm waggle", r,
                 {{Ignored, "outward", 44},
                  {Light, "guard", 156},
                  {Rejected, "too soon", 356},
                  {Light, "guard", 556},
                  {Rejected, "too soon", 756},
                  {Light, "guard", 956},
                  {Rejected, "too soon", 1156},
                  {Light, "guard", 1356},
                  {Rejected, "too soon", 1556},
                  {Light, "guard", 1756},
                  {Rejected, "too soon", 1956}});
    // Under every sampling condition: never heavy, and after the first half cycle no strike is
    // read as a wind-up (a stale rest pose after a stroke ended did that with dropped frames).
    const auto check = [](const Options& o, const std::string& what) {
        const Result rr = RunSwing(ArmWaggle(), 2.3, o);
        int lights = 0;
        for (const Event& e : rr.events) {
            EXPECT_NE(e.kind, Heavy) << what << ": " << Format(rr);
            EXPECT_FALSE(e.kind == Ignored && e.ms > 100.0) << what << ": " << Format(rr);
            lights += e.kind == Light ? 1 : 0;
        }
        EXPECT_GE(lights, 4) << what << ": " << Format(rr);
    };
    for (const int hz : {72, 90, 120}) {
        for (const int drop : {0, 3}) {
            Options o{};
            o.hz = hz;
            o.drop_every = drop;
            check(o, std::to_string(hz) + " Hz, drop every " + std::to_string(drop));
        }
    }
    for (const double p : {0.05, 0.10, 0.20}) {
        for (unsigned seed = 1; seed <= 20; ++seed) {
            Options o{};
            o.drop_p = p;
            o.seed = seed;
            check(o, std::to_string(p) + " random drops, seed " + std::to_string(seed));
        }
    }
}

TEST(SwingArcTest, FastHeaviesKeepTheirArc) {
    // Blade rates above GlitchRate (35 rad/s) are real swings, not pose jumps.
    for (const int hz : {90, 120}) {
        for (const double T : {0.15, 0.13, 0.12}) {
            Options o{};
            o.hz = hz;
            const Result r = RunSwing(Overhead(T), 0.9, o);
            EXPECT_EQ(Attacks(r), "R2")
                << "170 deg in " << T << " s, " << hz << " Hz: " << Format(r);
        }
        for (const double T : {0.17, 0.15}) {
            Options o{};
            o.hz = hz;
            const Result r =
                RunSwing(Rot(BODY, LEFT, 190, T, 0.10, At(BODY, 0.55, Az(120)), Az(135)), 0.9, o);
            EXPECT_EQ(Attacks(r), "R2")
                << "190 deg in " << T << " s, " << hz << " Hz: " << Format(r);
        }
    }
    for (const int hz : {72, 90, 120}) {
        for (const double snap : {0.10, 0.07, 0.05}) {
            Options o{};
            o.hz = hz;
            const Result r = RunSwing(WristSnapHeavy(snap), 0.9, o);
            EXPECT_EQ(Attacks(r), "R2")
                << "wrist snap " << snap << " s, " << hz << " Hz: " << Format(r);
        }
    }
}

TEST(SwingArcTest, OneFrameGlitchesNeverAddArc) {
    // A 120 deg wound-up chop stays light whatever one bad orientation frame at its peak does.
    for (const double deg : {15.0, 20.0, 25.0, 30.0, 45.0, 90.0}) {
        for (const int hz : {72, 90, 120}) {
            Options o{};
            o.hz = hz;
            const Result r = RunSwing(GlitchedChop(deg, hz, DOWN), 0.8, o);
            EXPECT_EQ(Attacks(r), "R1") << deg << " deg, " << hz << " Hz: " << Format(r);
        }
        const Result against = RunSwing(GlitchedChop(deg, 90, UP), 0.8);
        EXPECT_EQ(Attacks(against), "R1") << deg << " deg against the swing: " << Format(against);
    }
    // A 60 deg flip at rest is no stroke at all.
    const Traj flip = [](double t) {
        Sample s = Guard(t);
        if (t >= 0.50 && t < 0.511) {
            s.b = Rod(s.b, Unit({0.0, 1.0, 1.0}), 60.0 * Rad);
        }
        return s;
    };
    const Result r = RunSwing(flip, 1.0);
    EXPECT_TRUE(r.events.empty()) << Format(r);
}

TEST(SwingArcTest, NotASwing) {
    // A quick body turn with the weapon held in guard carries the hand around the head.
    for (const auto& [deg, T] : {std::pair{90.0, 0.6}, std::pair{120.0, 0.5}}) {
        for (const double sign : {1.0, -1.0}) {
            Options o{};
            o.turn = true;
            o.turn_deg = sign * deg;
            o.turn_s = T;
            o.noise = 0.2;
            const Result r = RunSwing(TurnInGuard(sign * deg, T), T + 0.6, o);
            EXPECT_EQ(Attacks(r), "") << sign * deg << " deg in " << T << " s: " << Format(r);
            o.arc = false;
            const Result off = RunSwing(TurnInGuard(sign * deg, T), T + 0.6, o);
            EXPECT_EQ(Attacks(off), "") << sign * deg << " deg, arc off: " << Format(off);
        }
    }
    // The headset slides 25 cm in 55 ms (re-localization): head-relative speed only.
    Options slide{};
    slide.head_offset = [](double t) {
        return D3{0.25 * std::clamp((t - 0.30) / 0.056, 0.0, 1.0), 0.0, 0.0};
    };
    slide.head_velocity = [](double t) {
        return t >= 0.30 && t < 0.356 ? D3{0.25 / 0.056, 0.0, 0.0} : D3{};
    };
    const Result r = RunSwing(Guard, 1.0, slide);
    EXPECT_EQ(Attacks(r), "") << Format(r);
    // The runtime reports a 13 m/s grip velocity for a few frames while the hand barely moves.
    for (const int frames : {2, 4, 6}) {
        const Traj spike = [frames](double t) {
            Sample s = Guard(t);
            if (t >= 0.50 && t < 0.50 + frames / 90.0 - 1e-6) {
                s.v = {12.0, 2.0, -6.0};
                s.p = s.p + D3{0.2, 0.0, 0.0};
            }
            return s;
        };
        const Result rs = RunSwing(spike, 1.0);
        EXPECT_EQ(Attacks(rs), "") << frames << " frames: " << Format(rs);
    }
    // Pulling the weapon back toward the body fast never attacks; chops from level still do.
    for (const int hz : {72, 90, 120}) {
        Options o{};
        o.hz = hz;
        const Result back = RunSwing(PullBack(), 0.6, o);
        EXPECT_EQ(Attacks(back), "") << hz << " Hz: " << Format(back);
        for (const double T : {0.22, 0.30, 0.40}) {
            const Result chop = RunSwing(Rot(SH, DOWN, 70, T, 0.10, At(SH, 0.5, El(0)), El(0)), 0.9, o);
            EXPECT_EQ(Attacks(chop), "R1") << T << " s chop, " << hz << " Hz: " << Format(chop);
        }
    }
}

TEST(SwingArcTest, BackhandFromFarLeftIsWoundUp) {
    // Mirror of the forehand from Az(+90): a wide backhand sweep is a heavy.
    for (const int hz : {72, 90, 120}) {
        for (const double az : {-90.0, -95.0, -100.0}) {
            Options o{};
            o.hz = hz;
            const Result r = RunSwing(
                Rot(BODY, RIGHT, 170, 0.35, 0.10, At(BODY, 0.55, Az(az)), Az(az - 5.0)), 0.8, o);
            EXPECT_EQ(Attacks(r), "R2") << "from Az(" << az << "), " << hz << " Hz: " << Format(r);
        }
        // ... and its short version stays light.
        Options o{};
        o.hz = hz;
        const Result light =
            RunSwing(Rot(BODY, RIGHT, 100, 0.22, 0.10, At(BODY, 0.45, Az(-90)), Az(-95)), 0.6, o);
        EXPECT_EQ(Attacks(light), "R1") << hz << " Hz: " << Format(light);
    }
}

TEST(SwingArcTest, DrawnDuringTrackingLoss) {
    // The weapon is drawn while the headset is untracked and the big swing starts as tracking
    // returns: the new stroke re-arms the gate.
    Options o{};
    o.head_lost[0] = 0.0;
    o.head_lost[1] = 0.25;
    const Rot swing = Overhead(0.35, 170, 0.30);
    const Result r = RunSwing(swing, 1.0, o);
    EXPECT_EQ(Attacks(r), "R2") << Format(r);
}

TEST(SwingArcTest, ImmersionFlickersKeepAPendingSwing) {
    // One frame (or 100 ms) without immersion during T05's pending decision changes nothing.
    for (const int hz : {72, 90, 120}) {
        for (const double length : {1.0 / hz, 0.10}) {
            Options o{};
            o.hz = hz;
            o.not_immersive[0] = 0.25;
            o.not_immersive[1] = 0.25 + length;
            const Result r = RunSwing(Overhead(), 0.8, o);
            EXPECT_EQ(Attacks(r), "R2") << length << " s at " << hz << " Hz: " << Format(r);
        }
    }
    // Lost for good: the pending swing is decided after InactiveHold, from the arc so far.
    Options lost{};
    lost.not_immersive[0] = 0.20;
    lost.not_immersive[1] = 9.0;
    ExpectEvents("slow T05, immersion lost", RunSwing(Overhead(0.60), 1.4, lost),
                 {{Light, "not immersive", 244}});
}

TEST(SwingArcTest, TurnCompensationKeepsABodyTurnLight) {
    // T19 without the head turn: the LOCAL sweep (~170 deg) alone would be a heavy.
    const Result r = RunSwing(BodyTurnSweep(), 0.8);
    EXPECT_EQ(Attacks(r), "R2") << Format(r);
}

TEST(SwingArcTest, BoundaryBetween135And145Degrees) {
    // T22: cocked overhead of varying size, time warp, orientation noise, 20 seeds.
    for (const double g : {0.7, 1.0, 1.5}) {
        for (const double deg : {110.0, 120.0, 130.0, 135.0, 140.0, 145.0, 150.0, 160.0, 170.0}) {
            for (const int hz : {72, 90}) {
                int heavies = 0;
                for (unsigned seed = 0; seed < 20; ++seed) {
                    Options o{};
                    o.hz = hz;
                    o.noise = 0.3;
                    o.seed = seed;
                    const Result r = RunSwing(Overhead(0.30, deg, 0.10, 0.5, 110, 130, g), 0.8, o);
                    const std::string attacks = Attacks(r);
                    heavies += attacks.rfind("R2", 0) == 0 ? 1 : 0;
                }
                if (deg <= 135.0) {
                    EXPECT_EQ(heavies, 0) << deg << " deg, g " << g << ", " << hz << " Hz";
                } else if (deg >= 145.0) {
                    EXPECT_EQ(heavies, 20) << deg << " deg, g " << g << ", " << hz << " Hz";
                }
            }
        }
    }
}

TEST(SwingArcTest, TriggerInterplay) {
    // T27a: trigger held from before the swing: the commit goes to the trigger machine (R2).
    Options busy{};
    busy.heavy_busy = true;
    ExpectEvents("T27a trigger + light swing", RunSwing(Forehand(), 0.6, busy), {{Commit, "", 78}});
    // T27b: charging: the big swing only releases the charge, no arc classification.
    const Result charge = RunSwing(Overhead(), 0.8, busy);
    ASSERT_EQ(charge.events.size(), 1u) << Format(charge);
    EXPECT_EQ(charge.events[0].kind, Commit) << Format(charge);
    // T27c: trigger squeezed while a wound-up short chop is pending: heavy at once.
    Options squeeze{};
    squeeze.trigger_at = 0.22;
    const Result r = RunSwing(CockedShortChop(), 0.6, squeeze);
    ExpectEvents("T27c trigger during a pending decision", r, {{Heavy, "trigger", 122}});
    EXPECT_TRUE(!r.events.empty() && r.events[0].consume) << Format(r);
}

TEST(SwingArcTest, GateVerdictKeepsThePendingDecision) {
    // A slow T05 blade (0.5 s) with a scripted hand: 2.6 m/s, a dip to 1 m/s (the gate
    // re-arms), 2.6 m/s again. The second commit lands while the first is pending and is
    // rejected ("too soon"); a trigger squeeze on that same frame still decides the pending
    // swing.
    const Rot swing = Overhead(0.5);
    const M::Config cfg = M::Config::Make(true, true, 140.0f, 100.0f, false, 0.0f);
    const D3 dir = Unit({0.0, -0.5, -1.0});
    const auto run = [&](int trigger_frame, std::vector<M::Output>& outputs) {
        SwingTestClassifier classifier;
        D3 p{0.25, -0.35, -0.35};
        for (int k = 1; k <= 60; ++k) {
            const double t = k / 90.0;
            const double speed = (t >= 0.20 && t < 0.27) || (t >= 0.295 && t < 0.45) ? 2.6
                                 : (t >= 0.27 && t < 0.295)                          ? 1.0
                                                                                     : 0.0;
            p = p + dir * (speed / 90.0);
            M::Input in{};
            in.t = t;
            in.active = true;
            in.trigger_heavy_edge = k == trigger_frame;
            in.ori_valid = true;
            in.q = GripFor(swing(t).b);
            in.pos_valid = true;
            in.p = F(D3{0.0, 1.6, 0.0} + p);
            in.v = F(dir * speed);
            in.head_ok = true;
            in.head_p = {0.0f, 1.6f, 0.0f};
            outputs.push_back(classifier.Update(in, cfg));
        }
    };
    std::vector<M::Output> plain;
    run(-1, plain);
    int rejected = -1;
    bool pending_before = false;
    for (size_t i = 0; i < plain.size(); ++i) {
        if (plain[i].gate == Rejected) {
            rejected = static_cast<int>(i) + 1;
            pending_before = i > 0 && plain[i - 1].sustain > 0.0f;
            break;
        }
    }
    ASSERT_GT(rejected, 0);
    EXPECT_TRUE(pending_before);
    EXPECT_EQ(plain[rejected - 1].kind, M::Kind::None);
    std::vector<M::Output> squeezed;
    run(rejected, squeezed);
    const M::Output& out = squeezed[rejected - 1];
    EXPECT_EQ(out.gate, Rejected);
    EXPECT_EQ(out.kind, Heavy);
    EXPECT_EQ(std::string(out.reason), "trigger");
    EXPECT_TRUE(out.consume_trigger);
}

TEST(SwingArcTest, GripReleasedWhilePending) {
    // T28: the decision is made at once from the arc so far (~63 deg): light.
    Options o{};
    o.release_at = 0.25;
    const Result r = RunSwing(Overhead(), 0.8, o);
    ExpectEvents("T28 grip released while pending", r, {{Light, "released", 150}});
    EXPECT_FALSE(r.events.empty());
    if (!r.events.empty()) {
        EXPECT_NEAR(r.events[0].arc, 63.0f, 8.0f) << Format(r);
    }
}

TEST(SwingArcTest, ReadyClickOnceWhenWoundUp) {
    // T09: raising the blade behind the head clicks once the hand settles, not during the raise.
    const Result r = RunSwing(RaiseOnly(), 0.9);
    ASSERT_EQ(r.clicks.size(), 1u) << Format(r);
    EXPECT_GT(r.clicks[0], 89.0);
    // A swing from guard never clicks.
    EXPECT_TRUE(RunSwing(Forehand(), 0.6).clicks.empty());
}

TEST(SwingArcTest, ArcOffIsTheOldDetector) {
    // heavy_arc=0 (or no grip pose): every swing is light and the old direction gates apply.
    Options off{};
    off.arc = false;
    const Result overhead = RunSwing(Overhead(), 0.8, off);
    EXPECT_EQ(Attacks(overhead), "R1") << Format(overhead);
    ExpectEvents("T09b, arc off", RunSwing(SideWindup(), 0.9, off), {{Light, "arc off", 89}});
    ExpectEvents("T16, arc off", RunSwing(DownChop(), 0.6, off), {{Ignored, "toward the body", 89}});
}

TEST(SwingArcTest, ResetDropsAPendingDecision) {
    // A context switch while a wound-up swing is pending drops it: no late attack, not even
    // the "released" decision when the weapon is sheathed afterwards.
    const Rot swing = Overhead();
    const M::Config cfg = M::Config::Make(true, true, 140.0f, 100.0f, false, 0.0f);
    SwingTestClassifier classifier;
    int events = 0;
    bool was_pending = false;
    for (int k = 1; k <= 72; ++k) {
        const double t = k / 90.0;
        const Sample s = swing(t);
        M::Input in{};
        in.t = t;
        in.active = !was_pending;
        in.ori_valid = true;
        in.q = GripFor(s.b);
        in.pos_valid = true;
        in.p = F(D3{0.0, 1.6, 0.0} + s.p);
        in.v = F(s.v);
        in.head_ok = true;
        in.head_p = {0.0f, 1.6f, 0.0f};
        const M::Output out = classifier.Update(in, cfg);
        events += out.kind == M::Kind::None ? 0 : 1;
        if (classifier.Pending() && !was_pending) {
            was_pending = true;
            classifier.Reset();
            EXPECT_FALSE(classifier.Pending());
        }
    }
    EXPECT_TRUE(was_pending);
    EXPECT_EQ(events, 0);
}

// ---- No grip clutch (right_grip=transform) ---------------------------------------------------

double FirstAttackMs(const Result& r) {
    for (const Event& e : r.events) {
        if (e.kind == M::Kind::Light || e.kind == M::Kind::Heavy) {
            return e.ms;
        }
    }
    return -1.0;
}

// A straight hand move of 45 cm forward with the blade turning by `turn_deg` (a reach for
// something, or a stab when fast).
Move Reach(double T, double turn_deg = 0.0) {
    return Move({0.20, -0.35, -0.15}, {0.20, -0.30, -0.60}, El(-10), El(-10 + turn_deg), T, 0.10);
}

TEST(SwingArcTest, ClutchlessKeepsEverySwing) {
    // Every reference trajectory gives the same attacks without the clutch, the first at most
    // two frames later; wind-ups, waggles, stirring and pull-backs still never attack.
    for (const Case& c : Cases()) {
        for (const int hz : {72, 90, 120}) {
            Options o = c.options;
            o.hz = hz;
            const Result held = RunSwing(c.traj, c.dur, o);
            o.clutchless = true;
            const Result free = RunSwing(c.traj, c.dur, o);
            EXPECT_EQ(Attacks(free), Attacks(held))
                << c.name << ", " << hz << " Hz: " << Format(free) << " | held: " << Format(held);
            const double a = FirstAttackMs(held);
            const double b = FirstAttackMs(free);
            if (a >= 0.0 && b >= 0.0) {
                EXPECT_LE(b - a, 2000.0 / hz + 1.0) << c.name << ", " << hz << " Hz";
            }
        }
    }
}

TEST(SwingArcTest, ClutchlessReachIsNotASwing) {
    // A quick reach forward (2.1 .. 2.8 m/s, the blade turning at most 10 deg): the faster ones
    // attacked with the grip held; without the clutch none does. A stab (3.4 m/s) still does.
    for (const int hz : {72, 90, 120}) {
        for (const double T : {0.30, 0.35, 0.40}) {
            for (const double turn : {0.0, 10.0}) {
                Options o{};
                o.hz = hz;
                o.noise = 0.2;
                const Result held = RunSwing(Reach(T, turn), T + 0.5, o);
                if (T < 0.38) {
                    EXPECT_EQ(Attacks(held), "R1") << T << " s, " << hz << " Hz: " << Format(held);
                }
                o.clutchless = true;
                const Result free = RunSwing(Reach(T, turn), T + 0.5, o);
                EXPECT_EQ(Attacks(free), "")
                    << T << " s, turn " << turn << ", " << hz << " Hz: " << Format(free);
            }
        }
        Options stab{};
        stab.hz = hz;
        stab.clutchless = true;
        const Result r = RunSwing(Reach(0.25), 0.8, stab);
        EXPECT_EQ(Attacks(r), "R1") << hz << " Hz: " << Format(r);
    }
    // The same reach out to the side (gesturing at something) and back.
    for (const double T : {0.30, 0.40}) {
        Options o{};
        o.clutchless = true;
        const Move out({0.20, -0.35, -0.30}, {0.62, -0.30, -0.30}, Az(0), Az(10), T, 0.10);
        const Move back(out.hb, out.ha, Az(10), Az(0), T, 0.20 + T);
        const Result r = RunSwing(Seq({{out, 0.20 + T - 1e-6}, {back, 9.0}}), 2.0 * T + 0.6, o);
        EXPECT_EQ(Attacks(r), "") << T << " s: " << Format(r);
    }
}

// ---- Overhead swings (QA 2026-10-07) ------------------------------------------------------------

int AttackCount(const Result& r) {
    int n = 0;
    for (const Event& e : r.events) {
        n += e.kind == M::Kind::Light || e.kind == M::Kind::Heavy ? 1 : 0;
    }
    return n;
}

TEST(SwingArcTest, WindUpsLoopedIntoTheChopAttackOnce) {
    // A wind-up that flows into its chop without the blade's rotation reversing (one stroke): the
    // gate ignored the chop as the wind-up and stayed disarmed (the session of 2026-10-07: nine
    // windows aborted "not a swing (ignored)"). Now one attack, at every rate and speed, with
    // the grip held or clutchless.
    struct Case {
        const char* name;
        std::vector<PathKey> keys;
        std::vector<double> speeds; // time scales
        bool lost_before;           // the old rules never attacked
    };
    // (Looped behind the head at x1.4 the chop takes 0.35 s with the hand below 3 m/s: lowering
    // the weapon, RecoverStrikeHand, with or without these rules.)
    const Case cases[] = {
        {"looped over the right shoulder", LoopOverShoulderKeys(), {0.7, 1.0, 1.4}, true},
        {"looped behind the head", LoopBehindHeadKeys(), {0.7, 1.0, 1.2}, false},
        {"slowly cocked back, then the chop (window #17)",
         SlowCockIntoChopKeys(),
         {0.7, 1.0, 1.4},
         true}};
    for (const Case& c : cases) {
        for (const int hz : {72, 90, 120}) {
            for (const double k : c.speeds) {
                for (const bool clutchless : {false, true}) {
                    Options o{};
                    o.hz = hz;
                    o.clutchless = clutchless;
                    o.noise = 0.2;
                    const std::vector<PathKey> keys = Timed(c.keys, 0.10, k);
                    const Result r = RunSwing(Keyed(keys), keys.back().t + 0.6, o);
                    EXPECT_EQ(AttackCount(r), 1)
                        << c.name << ", x" << k << ", " << hz << " Hz"
                        << (clutchless ? ", clutchless" : "") << ": " << Format(r);
                }
            }
        }
    }
    // Without the overhead rules (swing_windup_turn_deg=0, swing_toward_front_deg=0,
    // swing_windup_rejudge=0) the looped chops were lost.
    for (const Case& c : cases) {
        if (!c.lost_before) {
            continue;
        }
        Options old{};
        old.clutchless = true;
        old.overhead = false;
        const std::vector<PathKey> keys = Timed(c.keys, 0.10, 1.0);
        const Result r = RunSwing(Keyed(keys), keys.back().t + 0.6, old);
        EXPECT_EQ(AttackCount(r), 0) << c.name << ", the old rules: " << Format(r);
    }
}

TEST(SwingArcTest, LoopedWindUpsAloneNeverAttack) {
    // The same loops held at the top, or brought back down to the guard slowly (0.8 s): wind-ups
    // and a recovery, never an attack.
    for (const int hz : {72, 90, 120}) {
        for (const double k : {0.7, 1.0, 1.4}) {
            for (const bool clutchless : {false, true}) {
                Options o{};
                o.hz = hz;
                o.clutchless = clutchless;
                o.noise = 0.2;
                const std::vector<PathKey> held = Timed(LoopOnlyKeys(), 0.10, k);
                const Result r = RunSwing(Keyed(held), held.back().t + 0.8, o);
                EXPECT_EQ(Attacks(r), "") << "held, x" << k << ", " << hz << " Hz: " << Format(r);
                std::vector<PathKey> lowered = held;
                const PathKey top = lowered.back();
                lowered.push_back({top.t + 0.3, top.p, top.b});
                lowered.push_back({top.t + 1.1, GuardHand, El(40)});
                const Result l = RunSwing(Keyed(lowered), lowered.back().t + 0.5, o);
                EXPECT_EQ(Attacks(l), "")
                    << "lowered, x" << k << ", " << hz << " Hz: " << Format(l);
            }
        }
    }
}

TEST(SwingArcTest, AStrokeIgnoredAsAWindUpCommitsOnlyAsAStrike) {
    // Judged again while it lasts: a raise past the vertical whose blade goes on down behind
    // (T09) gives no second verdict; the pull to the face (T23) none either; the waggle's first
    // half stays the only wind-up (ArmWaggleIsNeverHeavy).
    ExpectEvents("T09 raise only", RunSwing(RaiseOnly(), 0.9), {{Ignored, "raising", 89}});
    ExpectEvents("T23 pull hand to face", RunSwing(PullToFace(), 0.6), {{Ignored, "raising", 78}});
    // The hand pulled to the face and struck straight back out (the face gate's business in the
    // presenter): the second stroke is a strike with or without these rules.
    Options o{};
    o.clutchless = true;
    const Move in({0.25, -0.35, -0.55}, {0.10, -0.10, -0.10}, El(10), El(80), 0.20, 0.10);
    const Move out(in.hb, in.ha, El(80), El(10), 0.25, 0.30);
    const Traj both = Seq({{in, 0.30 - 1e-6}, {out, 9.0}});
    const Result now = RunSwing(both, 1.0, o);
    o.overhead = false;
    const Result before = RunSwing(both, 1.0, o);
    EXPECT_EQ(Attacks(now), Attacks(before)) << Format(now) << " | before: " << Format(before);
}

} // namespace
