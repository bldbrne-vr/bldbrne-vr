// SPDX-License-Identifier: GPL-2.0-or-later

// Dodge comfort camera (core/vr/motion_camera.h): the motion clip classification, the gate that
// picks the camera mode, and the comfort camera driven at 60 Hz with alternating stereo eyes (the
// first eye of a pair at 30 Hz) or in mono. The dodge profiles are synthetic: a root path whose
// speed per 0.1 s is shaped like the clip's root-speed table, keyed at 30 Hz like the game's root
// motion and linear in between.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/motion_camera.h"

namespace {

namespace M = VR::Motion;

constexpr s64 Ms = M::Millis;
constexpr s64 Frame = 16'666'667; // one render-camera tick at 60 fps
constexpr s64 Start = 10'000 * Ms;

// ---- Synthetic root paths --------------------------------------------------------------------

struct RootPath {
    std::vector<float> speeds; // m/s per 0.1 s

    // Exact distance of the piecewise-constant speed.
    double Distance(double t) const {
        double d = 0.0;
        for (size_t i = 0; i < speeds.size(); ++i) {
            const double a = 0.1 * static_cast<double>(i);
            if (t <= a) {
                break;
            }
            d += speeds[i] * (std::min(t, a + 0.1) - a);
        }
        return d;
    }
    // Root motion keyed at 30 Hz, linear between the keys.
    double At(double t) const {
        if (t <= 0.0) {
            return 0.0;
        }
        const double k = std::floor(t * 30.0);
        const double f = t * 30.0 - k;
        return Distance(k / 30.0) * (1.0 - f) + Distance((k + 1.0) / 30.0) * f;
    }
    double Length() const {
        return 0.1 * static_cast<double>(speeds.size());
    }
};

// Shaped like the root-speed tables of a000 7100 (quickstep forward), 7102 (quickstep left), 7200
// (roll) and 7000 (backstep).
const RootPath Quickstep{
    {1.0f, 8.8f, 9.6f, 7.1f, 5.4f, 4.3f, 2.6f, 1.1f, 0.4f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f}};
const RootPath QuickstepLeft{
    {2.3f, 9.6f, 8.8f, 7.3f, 5.0f, 3.9f, 2.2f, 1.2f, 0.1f, 0.1f, 0.1f, 0.1f}};
const RootPath Roll{{7.9f, 7.9f, 7.9f, 6.6f, 3.1f, 1.8f, 2.5f, 1.4f, 0.7f, 0.1f}};
const RootPath Backstep{{4.3f, 6.1f, 5.5f, 3.8f, 2.5f, 1.3f, 0.8f, 0.5f, 0.2f, 0.1f}};

// A descending staircase: height for a horizontal distance (step function).
float Stairs(double distance, double riser = 0.17, double tread = 0.30) {
    return static_cast<float>(-riser * std::floor(distance / tread));
}

constexpr float Eye = 1.6f; // eye height above the feet (the target already includes it)

// ---- Simulation --------------------------------------------------------------------------------

struct Tick {
    s64 t;
    bool first;
    M::V3 root;
    M::ComfortOut out;
};

struct Scenario {
    std::function<M::V3(double)> root;     // seconds -> target (game units)
    std::function<M::Phase(double)> phase; // seconds -> the gate's phase
    double seconds{3.0};
    bool stereo{true}; // alternate eyes (the first eye at 30 Hz); false = mono
    std::function<const char*(double)> reset; // optional reset reason per tick
};

std::vector<Tick> RunScenario(const Scenario& sc, const M::ComfortConfig& cfg, M::ComfortCamera* camera) {
    M::ComfortCamera local;
    M::ComfortCamera& cam = camera != nullptr ? *camera : local;
    std::vector<Tick> ticks;
    const s64 n = static_cast<s64>(sc.seconds * 60.0);
    for (s64 i = 0; i <= n; ++i) {
        const s64 t = Start + i * Frame;
        const double s = static_cast<double>(i * Frame) * 1e-9;
        const bool first = !sc.stereo || i % 2 == 0;
        const M::V3 root = sc.root(s);
        const char* reset = sc.reset ? sc.reset(s) : nullptr;
        ticks.push_back({t, first, root, cam.Step(root, first, t, sc.phase(s), cfg, reset)});
    }
    return ticks;
}

std::vector<Tick> RunScenario(const Scenario& sc, const M::ComfortConfig& cfg = {}) {
    return RunScenario(sc, cfg, nullptr);
}

M::Phase Mode(M::Mode mode, bool fall_confirmed = false) {
    M::Phase p{};
    p.stale = false;
    p.mode = mode;
    p.fall_confirmed = fall_confirmed;
    return p;
}

// A dodge of `clip` that started `since` seconds ago (negative: not yet), active for `length` s.
M::Phase Dodge(s32 clip, double since, double length) {
    if (since < 0.0 || since >= length) {
        return Mode(M::Mode::Ground);
    }
    M::Phase p = Mode(M::Mode::Dodge);
    p.dodging = true;
    p.dodge = M::ClassifyMotionClip(clip);
    p.dodge_clip = clip;
    p.dodge_ns = static_cast<s64>(since * 1e9);
    return p;
}

// Metrics over the first-eye sequence (what the headset shows each pair), in meters.
struct Metrics {
    float lag{0.0f};     // largest |camera - body|
    float over{0.0f};    // camera past the body along its net movement
    float settle{-1.0f}; // s: the body's last movement -> error under 1 cm for good (-1 never)
    float amax{0.0f};    // m/s^2
    float vmax{0.0f};    // m/s
    bool constant{true}; // the camera never moved on this axis
};

// `axis`: projects a position onto the measured axis (horizontal: along the dodge).
Metrics Measure(const std::vector<Tick>& ticks, const std::function<float(const M::V3&)>& axis,
                float scale = 1.0f) {
    std::vector<const Tick*> f;
    for (const auto& tick : ticks) {
        if (tick.first) {
            f.push_back(&tick);
        }
    }
    Metrics m{};
    if (f.size() < 3) {
        return m;
    }
    const float inv = 1.0f / scale;
    const float net = axis(f.back()->root) - axis(f.front()->root);
    const float sign = net >= 0.0f ? 1.0f : -1.0f;
    size_t last_move = 0;
    float prev_v = 0.0f;
    const float first_cam = axis(f.front()->out.position);
    for (size_t i = 0; i < f.size(); ++i) {
        const float r = axis(f[i]->root);
        const float c = axis(f[i]->out.position);
        m.lag = std::max(m.lag, std::abs(c - r) * inv);
        m.over = std::max(m.over, sign * (c - r) * inv);
        if (c != first_cam) {
            m.constant = false;
        }
        if (i == 0) {
            continue;
        }
        const float dt = static_cast<float>(f[i]->t - f[i - 1]->t) * 1e-9f;
        if (std::abs(r - axis(f[i - 1]->root)) * inv / dt > 0.2f) { // the body moves
            last_move = i;
        }
        const float v = (c - axis(f[i - 1]->out.position)) * inv / dt;
        m.vmax = std::max(m.vmax, std::abs(v));
        if (i >= 2) {
            m.amax = std::max(m.amax, std::abs(v - prev_v) / dt);
        }
        prev_v = v;
    }
    size_t settled = f.size();
    while (settled > 0 &&
           std::abs(axis(f[settled - 1]->out.position) - axis(f[settled - 1]->root)) * inv <
               0.01f) {
        --settled;
    }
    if (settled < f.size()) {
        const size_t at = std::max(settled, last_move);
        m.settle = static_cast<float>(f[at]->t - f[last_move]->t) * 1e-9f;
    }
    return m;
}

float Y(const M::V3& v) {
    return v.y;
}

// ---- Classification --------------------------------------------------------------------------

TEST(MotionClip, Classification) {
    EXPECT_EQ(M::ClassifyMotionClip(7000).kind, M::Kind::Backstep);
    EXPECT_EQ(M::ClassifyMotionClip(7000).committed_ms, 700);
    const M::ClipInfo left = M::ClassifyMotionClip(2007102); // weapon category 2
    EXPECT_EQ(left.kind, M::Kind::Quickstep);
    EXPECT_EQ(left.dir, 2);
    EXPECT_EQ(std::string(M::DirName(left.dir)), std::string("L"));
    const M::ClipInfo ohb = M::ClassifyMotionClip(7155);
    EXPECT_EQ(ohb.kind, M::Kind::QuickstepOhb);
    EXPECT_EQ(std::string(M::DirName(ohb.dir)), std::string("FR"));
    EXPECT_EQ(M::ClassifyMotionClip(7200).kind, M::Kind::Roll);
    EXPECT_EQ(M::ClassifyMotionClip(7200).dip_ms, 700);
    EXPECT_EQ(M::ClassifyMotionClip(7457).kind, M::Kind::WakeRoll);
    EXPECT_EQ(M::ClassifyMotionClip(7800).kind, M::Kind::Jump);
    EXPECT_EQ(M::ClassifyMotionClip(8050).kind, M::Kind::JumpFall);
    EXPECT_EQ(M::ClassifyMotionClip(8020).kind, M::Kind::FallStart);
    EXPECT_EQ(M::ClassifyMotionClip(8120).kind, M::Kind::Falling);
    EXPECT_EQ(M::ClassifyMotionClip(8250).kind, M::Kind::Land);
    EXPECT_EQ(M::ClassifyMotionClip(5000).kind, M::Kind::Sprint);
    EXPECT_EQ(M::ClassifyMotionClip(20410).kind, M::Kind::Knockdown);
    EXPECT_EQ(M::ClassifyMotionClip(20100).kind, M::Kind::Damage);
    EXPECT_EQ(M::ClassifyMotionClip(101011).kind, M::Kind::Ladder);
    EXPECT_FALSE(M::ClassifyMotionClip(101011).ladder_end);
    EXPECT_TRUE(M::ClassifyMotionClip(101013).ladder_end);           // getting off at the top
    EXPECT_EQ(M::ClassifyMotionClip(100033000).kind, M::Kind::None); // a throw
    EXPECT_EQ(M::ClassifyMotionClip(-1).kind, M::Kind::None);
    EXPECT_EQ(M::ClassifyMotionClip(30000).kind, M::Kind::None); // an attack
    EXPECT_EQ(M::ClassifyMotionClip(7108).kind, M::Kind::None);
}

// ---- Gate ------------------------------------------------------------------------------------

M::Signals Fresh(s64 now) {
    M::Signals s{};
    s.valid = true;
    s.update_ns = now;
    s.sp_valid = true;
    s.ring_valid = true;
    s.grounded_valid = true;
    s.grounded = true;
    s.ladder_valid = true;
    s.ladder = -1;
    return s;
}

struct GateDriver {
    M::Gate gate{};
    s64 t{Start};
    M::Signals s{Fresh(Start)};
    u32 serial{0};

    void Push(s32 clip) {
        s.clip = clip;
        s.clip_ns = t;
        s.clip_serial = ++serial;
    }
    M::Phase Tick() {
        s.update_ns = t;
        const M::Phase p = gate.Step(s, t);
        t += Frame;
        return p;
    }
    // Steps for `ms`; returns the last phase and counts the edges.
    M::Phase RunScenario(s64 ms, u32* starts = nullptr, u32* ends = nullptr,
                 const char** reason = nullptr) {
        M::Phase p{};
        const s64 end = t + ms * Ms;
        while (t < end) {
            p = Tick();
            if (starts != nullptr && p.dodge_start) {
                ++*starts;
            }
            if (ends != nullptr && p.dodge_end) {
                ++*ends;
                if (reason != nullptr) {
                    *reason = p.end_reason;
                }
            }
        }
        return p;
    }
};

TEST(MotionGate, SpEffectEdges) {
    GateDriver g;
    const M::Phase idle = g.Tick();
    EXPECT_EQ(idle.mode, M::Mode::Ground);
    g.s.sp392 = true;
    const M::Phase start = g.Tick();
    EXPECT_TRUE(start.dodge_start);
    EXPECT_EQ(start.mode, M::Mode::Dodge);
    EXPECT_EQ(start.sp_id, 392);
    EXPECT_EQ(start.dodge.kind, M::Kind::None); // no clip yet
    u32 starts = 0, ends = 0;
    const M::Phase during = g.RunScenario(500, &starts, &ends);
    EXPECT_EQ(during.mode, M::Mode::Dodge);
    EXPECT_EQ(starts, 0u);
    g.s.sp392 = false;
    const char* reason = "";
    const M::Phase after = g.RunScenario(100, &starts, &ends, &reason);
    EXPECT_EQ(after.mode, M::Mode::Ground);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(std::string(reason), std::string("sp off"));
    // Backsteps carry 393.
    g.s.sp393 = true;
    const M::Phase backstep = g.Tick();
    EXPECT_EQ(backstep.sp_id, 393);
}

TEST(MotionGate, TimerFallbackWithoutSpEffect) {
    GateDriver g;
    g.s.sp_valid = false;
    g.Push(7200); // roll, committed 870 ms
    const M::Phase start = g.Tick();
    EXPECT_TRUE(start.dodge_start);
    EXPECT_EQ(start.dodge.kind, M::Kind::Roll);
    EXPECT_EQ(start.dodge_clip, 7200);
    u32 ends = 0;
    const char* reason = "";
    g.RunScenario(800, nullptr, &ends, &reason);
    EXPECT_EQ(ends, 0u);
    g.RunScenario(150, nullptr, &ends, &reason);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(std::string(reason), std::string("timer"));
}

TEST(MotionGate, ClipNamesAnSpEffectStartAndChainsRestart) {
    GateDriver g;
    g.s.sp392 = true;
    EXPECT_TRUE(g.Tick().dodge_start); // the SpEffect came first
    g.Push(7200);
    M::Phase p = g.Tick();
    EXPECT_FALSE(p.dodge_start); // the clip names it, no restart
    EXPECT_EQ(p.dodge.kind, M::Kind::Roll);
    g.RunScenario(700);
    const s64 before = g.Tick().dodge_ns;
    EXPECT_GT(before, 700 * Ms);
    g.Push(7200); // a roll out of a roll
    p = g.Tick();
    EXPECT_TRUE(p.dodge_start);
    EXPECT_TRUE(p.chained);
    EXPECT_EQ(p.dodge_ns, 0);
    // The same push seen again changes nothing.
    EXPECT_FALSE(g.Tick().dodge_start);
}

TEST(MotionGate, FallByRingOrByNotLanded) {
    GateDriver g;
    g.Push(8000); // fall start
    M::Phase p = g.Tick();
    EXPECT_EQ(p.mode, M::Mode::Fall);
    EXPECT_TRUE(p.fall_confirmed);
    g.Push(8200); // landing
    p = g.Tick();
    EXPECT_EQ(p.mode, M::Mode::Ground);
    EXPECT_FALSE(p.fall_confirmed);

    // Not landed alone: Fall after 100 ms, never confirmed.
    g.s.grounded = false;
    const M::Phase early = g.RunScenario(80);
    EXPECT_EQ(early.mode, M::Mode::Ground);
    p = g.RunScenario(50);
    EXPECT_EQ(p.mode, M::Mode::Fall);
    EXPECT_FALSE(p.fall_confirmed);
    g.s.grounded = true;
    const M::Phase landed = g.Tick();
    EXPECT_EQ(landed.mode, M::Mode::Ground);

    // A confirmed fall ends after 200 ms on the ground even without a landing clip.
    g.Push(8100);
    EXPECT_TRUE(g.Tick().fall_confirmed);
    EXPECT_TRUE(g.RunScenario(150).fall_confirmed);
    EXPECT_FALSE(g.RunScenario(100).fall_confirmed);
    // A dodge during a confirmed fall stays Fall (a roll off a ledge follows the drop).
    g.Push(8100);
    g.s.grounded = false;
    g.Tick();
    g.s.sp392 = true;
    p = g.Tick();
    EXPECT_TRUE(p.dodging);
    EXPECT_EQ(p.mode, M::Mode::Fall);
}

TEST(MotionGate, LadderBeatsFall) {
    GateDriver g;
    g.s.grounded = false;
    g.s.ladder = 2; // a step up
    const M::Phase climbing = g.RunScenario(300);
    EXPECT_EQ(climbing.mode, M::Mode::Ladder);
    // A climbing clip keeps Ladder while the state byte reads -1 (a damage bind) ...
    g.Push(101060);
    g.s.ladder = -1;
    const M::Phase bound = g.RunScenario(500);
    EXPECT_EQ(bound.mode, M::Mode::Ladder);
    // ... for at most a second.
    const M::Phase off_long = g.RunScenario(600);
    EXPECT_EQ(off_long.mode, M::Mode::Fall);
    // Getting off ends it at once.
    g.s.ladder = 8;
    g.Push(101011);
    const M::Phase climbing_again = g.Tick();
    EXPECT_EQ(climbing_again.mode, M::Mode::Ladder);
    g.s.ladder = -1;
    g.Push(101013); // exit at the top
    const M::Phase dismount = g.Tick();
    EXPECT_EQ(dismount.mode, M::Mode::Fall); // not landed: the dismount follows like a fall
}

TEST(MotionGate, LadderStartTimeSurvivesRungsAndEndsOnDismount) {
    for (const s8 mount : {s8{0}, s8{1}}) { // getting on from below and above
        GateDriver g;
        EXPECT_EQ(g.Tick().ladder_start_ns, 0);
        g.s.ladder = mount;
        const s64 started = g.t;
        EXPECT_EQ(g.Tick().ladder_start_ns, started);
        for (const s8 rung : {s8{2}, s8{3}, s8{4}, s8{5}, s8{8}, s8{9}}) {
            g.s.ladder = rung;
            g.Push(101011);
            EXPECT_EQ(g.RunScenario(300).ladder_start_ns, started);
        }
        g.s.ladder = -1;
        g.Push(101013);
        EXPECT_EQ(g.Tick().ladder_start_ns, 0);
        g.s.ladder = mount;
        const s64 restarted = g.t;
        EXPECT_EQ(g.Tick().ladder_start_ns, restarted);
        EXPECT_GT(restarted, started);
    }
}

TEST(MotionGate, ClipOnlyLadderStartClearsOnDeathAndStaleSignals) {
    GateDriver g;
    g.s.ladder_valid = false;
    g.s.hp_valid = true;
    g.s.hp = 500;
    g.Push(101010);
    const s64 started = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, started);
    g.Push(101011);
    EXPECT_EQ(g.RunScenario(500).ladder_start_ns, started);
    g.s.hp = 0;
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.s.hp = 500;
    EXPECT_EQ(g.Tick().ladder_start_ns, 0); // an old clip after respawn is not a new climb
    g.Push(101010);
    const s64 restarted = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, restarted);
    EXPECT_EQ(g.gate.Step(g.s, g.t + 300 * Ms).ladder_start_ns, 0);
}

TEST(MotionGate, ResumingClimbingIsNotAnotherMount) {
    GateDriver g;
    g.s.ladder = 0;
    const s64 started = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, started);
    g.s.ladder = 2;
    g.Push(101011);
    EXPECT_EQ(g.RunScenario(4000).ladder_start_ns, started);
    // A damage bind can write -1 before its animation reaches the pending clip ring.
    g.s.ladder = -1;
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.s.ladder = 2;
    g.Push(101011);
    EXPECT_TRUE(g.Tick().ladder);
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.s.ladder = -1;
    g.Push(101013);
    g.Tick();
    g.s.ladder = 0;
    const s64 remount = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, remount);
}

TEST(MotionGate, AClimbWithoutAMountDoesNotInventAFadeWindow) {
    for (const s32 clip : {101011, 101030, 101035, 101060}) {
        GateDriver g;
        g.s.ladder = 8;
        g.Push(clip);
        EXPECT_TRUE(g.Tick().ladder);
        EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    }
}

TEST(MotionGate, DismountClipEndsFadeBeforeTheStateByteCatchesUp) {
    for (const s32 clip : {101013, 101023, 101002, 101050}) {
        GateDriver g;
        g.s.ladder = 0;
        EXPECT_NE(g.Tick().ladder_start_ns, 0);
        g.Push(clip);
        EXPECT_EQ(g.Tick().ladder_start_ns, 0);
        EXPECT_EQ(g.Tick().ladder_start_ns, 0); // still reading the old mount state
    }
}

TEST(MotionGate, RepeatedMountClipAfterAStaleGapDoesNotRestartFade) {
    GateDriver g;
    g.s.ladder_valid = false;
    g.Push(101010);
    const s64 started = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, started);
    g.t += 500 * Ms;
    EXPECT_TRUE(g.gate.Step(g.s, g.t).stale);
    g.Push(101010); // the same mount is re-published after the hitch
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.Push(101013);
    g.Tick();
    g.Push(101020);
    const s64 next_mount = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, next_mount);
}

TEST(MotionGate, MountStateAndClipShareOneTimerInEitherOrder) {
    for (const bool clip_first : {false, true}) {
        GateDriver g;
        const s64 started = g.t;
        if (clip_first) {
            g.Push(101010);
        } else {
            g.s.ladder = 0;
        }
        EXPECT_EQ(g.Tick().ladder_start_ns, started);
        g.t += 100 * Ms;
        if (clip_first) {
            g.s.ladder = 0;
        } else {
            g.Push(101010);
        }
        EXPECT_EQ(g.Tick().ladder_start_ns, started);
        for (int i = 0; i < 4; ++i) {
            g.t += 100 * Ms;
            g.Push(101010);
            EXPECT_EQ(g.Tick().ladder_start_ns, started);
        }
    }
}

TEST(MotionGate, DyingDuringMountDoesNotSuppressTheNextMount) {
    GateDriver g;
    g.s.ladder_valid = false;
    g.s.hp_valid = true;
    g.s.hp = 500;
    g.Push(101010);
    EXPECT_NE(g.Tick().ladder_start_ns, 0);
    g.s.hp = 0;
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.Push(101010); // still dead: never counted as the next mount
    EXPECT_EQ(g.Tick().ladder_start_ns, 0);
    g.s.hp = 500;
    g.Push(101010);
    const s64 next_mount = g.t;
    EXPECT_EQ(g.Tick().ladder_start_ns, next_mount);
}

TEST(MotionGate, LadderDeadLatch) {
    // Killed on a ladder: the death states never write the ladder state, so it keeps its climbing
    // value through the respawn's loading screen. HP <= 0 latches: that value is ignored (falls and
    // dodges get their own modes again) until the state changes or a climbing clip arrives while
    // alive.
    GateDriver g;
    g.s.hp_valid = true;
    g.s.hp = 500;
    g.s.ladder = 8; // climbing, idle
    g.Push(101011); // a step up
    EXPECT_EQ(g.RunScenario(300).mode, M::Mode::Ladder);
    g.s.hp = 0;
    M::Phase p = g.Tick();
    EXPECT_TRUE(p.ladder_dead);
    EXPECT_FALSE(p.ladder);
    // The respawn: no update for 3 s (stale), then alive again with the byte still 8.
    const M::Signals old = g.s;
    int stale = 0;
    for (int i = 0; i < 180; ++i) {
        g.t += Frame;
        stale += g.gate.Step(old, g.t).stale ? 1 : 0;
    }
    EXPECT_GE(stale, 160);
    g.s.hp = 500;
    p = g.RunScenario(100);
    EXPECT_TRUE(p.ladder_dead);
    EXPECT_FALSE(p.ladder);
    EXPECT_EQ(p.mode, M::Mode::Ground);
    g.s.grounded = false;
    EXPECT_EQ(g.RunScenario(150).mode, M::Mode::Fall);
    g.s.grounded = true;
    g.Push(8200);
    g.Tick();
    g.s.sp392 = true;
    EXPECT_EQ(g.RunScenario(50).mode, M::Mode::Dodge);
    g.s.sp392 = false;
    // A ladder clip that is no climb (an attack on the ladder) does not release it.
    g.Push(101035);
    EXPECT_TRUE(g.RunScenario(1500).ladder_dead);
    // The next ladder: getting on changes the state (0: from below): Ladder at once.
    g.s.ladder = 0;
    p = g.Tick();
    EXPECT_FALSE(p.ladder_dead);
    EXPECT_EQ(p.mode, M::Mode::Ladder);
    // Killed again; a climbing clip while alive releases it with the byte unchanged.
    g.s.ladder = 8;
    g.RunScenario(100);
    g.s.hp = 0;
    EXPECT_TRUE(g.RunScenario(100).ladder_dead);
    g.Push(101011); // dead: a clip does not release it
    EXPECT_TRUE(g.Tick().ladder_dead);
    g.s.hp = 500;
    EXPECT_FALSE(g.Tick().ladder);
    g.Push(101010); // getting on (from below)
    p = g.Tick();
    EXPECT_FALSE(p.ladder_dead);
    EXPECT_EQ(p.mode, M::Mode::Ladder);
    // Killed off a ladder (the byte -1): the next ladder's state releases it.
    g.s.ladder = -1;
    g.Push(101013); // getting off at the top
    g.RunScenario(1200);
    g.s.hp = 0;
    g.RunScenario(50);
    g.s.hp = 500;
    EXPECT_TRUE(g.RunScenario(500).ladder_dead);
    g.s.ladder = 1;
    EXPECT_EQ(g.Tick().mode, M::Mode::Ladder);
}

TEST(MotionGate, StaleSignalsAreGround) {
    GateDriver g;
    g.s.sp392 = true;
    EXPECT_TRUE(g.Tick().dodging);
    // No update for 300 ms (loading, no hook): no information.
    const M::Signals old = g.s;
    g.t += 300 * Ms;
    const M::Phase p = g.gate.Step(old, g.t);
    EXPECT_TRUE(p.stale);
    EXPECT_EQ(p.mode, M::Mode::Ground);
    EXPECT_TRUE(p.dodge_end);
    EXPECT_EQ(std::string(p.end_reason), std::string("stale"));
    // Never published at all.
    M::Gate fresh;
    const M::Phase never = fresh.Step(M::Signals{}, g.t);
    EXPECT_EQ(never.mode, M::Mode::Ground);
}

TEST(MotionGate, OldClipAfterLoadingIsNotNew) {
    GateDriver g;
    g.s.sp_valid = false;
    g.Push(8000);     // a fall start just before a loading screen
    g.t += 2000 * Ms; // the update stalled (loading) and comes back with the same clip
    const M::Phase back = g.Tick();
    EXPECT_FALSE(back.fall_confirmed);
    EXPECT_EQ(back.mode, M::Mode::Ground);
    g.Push(101011); // a climbing clip from before is not a climb either
    g.t += 2000 * Ms;
    const M::Phase later = g.Tick();
    EXPECT_FALSE(later.ladder);
}

TEST(MotionGate, CapEndsALongDodge) {
    GateDriver g;
    g.s.sp392 = true; // stuck on
    u32 starts = 0, ends = 0;
    const char* reason = "";
    g.RunScenario(5000, &starts, &ends, &reason);
    EXPECT_EQ(starts, 1u);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(std::string(reason), std::string("cap"));
    g.s.sp392 = false;
    g.Tick();
    g.s.sp392 = true;
    EXPECT_TRUE(g.Tick().dodge_start);
}

// ---- Comfort camera ----------------------------------------------------------------------------

// A dodge along `dir` (unit, xz) on flat ground: 0.3 s standing, the dodge, 1 s after.
Scenario FlatDodge(const RootPath& path, s32 clip, float dx, float dz, bool stereo) {
    Scenario sc{};
    sc.seconds = 0.3 + path.Length() + 1.0;
    sc.stereo = stereo;
    sc.root = [&path, dx, dz](double s) {
        const float d = static_cast<float>(path.At(s - 0.3));
        return M::V3{5.0f + dx * d, Eye, -3.0f + dz * d};
    };
    sc.phase = [clip](double s) { return Dodge(clip, s - 0.3, 0.8); };
    return sc;
}

TEST(ComfortCamera, FlatDodgesFollowWithoutVerticalMotion) {
    struct Case {
        const RootPath* path;
        s32 clip;
    };
    const Case cases[] = {
        {&Quickstep, 7100}, {&QuickstepLeft, 7102}, {&Roll, 7200}, {&Backstep, 7000}};
    for (const auto& c : cases) {
        for (const bool stereo : {true, false}) {
            const float dx = 0.6f, dz = -0.8f;
            const auto ticks = RunScenario(FlatDodge(*c.path, c.clip, dx, dz, stereo));
            const auto along = [dx, dz](const M::V3& v) { return v.x * dx + v.z * dz; };
            const Metrics h = Measure(ticks, along);
            EXPECT_LE(h.lag, 0.20f) << c.clip << " stereo " << stereo;
            EXPECT_LE(h.over, 0.005f) << c.clip << " stereo " << stereo;
            EXPECT_GE(h.settle, 0.0f) << c.clip;
            EXPECT_LE(h.settle, 0.050f) << c.clip << " stereo " << stereo;
            // The follow rounds off the 30 Hz steps of the root motion: a gentler peak
            // acceleration than the body's own.
            std::vector<Tick> raw = ticks;
            for (auto& t : raw) {
                t.out.position = t.root;
            }
            const Metrics body = Measure(raw, along);
            EXPECT_LT(h.amax, 0.9f * body.amax) << c.clip << " stereo " << stereo;
            const Metrics v = Measure(ticks, Y);
            EXPECT_TRUE(v.constant) << c.clip; // no animation vertical, bit for bit
            // The camera never leaves the dodge's line.
            float side = 0.0f;
            for (const auto& t : ticks) {
                const M::V3 d = t.out.position - M::V3{5.0f, Eye, -3.0f};
                side = std::max(side, std::abs(d.x * dz - d.z * dx));
            }
            EXPECT_LE(side, 1e-4f) << c.clip;
            EXPECT_FALSE(ticks.back().out.reset);
        }
    }
}

TEST(ComfortCamera, PairLockSharesTheFirstEyesPosition) {
    const auto ticks = RunScenario(FlatDodge(Quickstep, 7100, 1.0f, 0.0f, true));
    u32 locked = 0;
    for (size_t i = 1; i < ticks.size(); ++i) {
        if (!ticks[i].first) {
            EXPECT_TRUE(ticks[i].out.locked);
            EXPECT_EQ(
                std::memcmp(&ticks[i].out.position, &ticks[i - 1].out.position, sizeof(M::V3)), 0);
            ++locked;
        }
    }
    EXPECT_GT(locked, 50u);

    // A second eye more than 1.5 frames late is not locked: it moves with the body.
    M::ComfortCamera cam;
    const M::ComfortConfig cfg{};
    const M::Phase ground = Mode(M::Mode::Ground);
    cam.Step({0.0f, Eye, 0.0f}, true, Start, ground, cfg);
    const M::ComfortOut a = cam.Step({0.1f, Eye, 0.0f}, true, Start + Frame, ground, cfg);
    const M::ComfortOut late =
        cam.Step({0.4f, Eye, 0.0f}, false, Start + Frame + 30 * Ms, ground, cfg);
    EXPECT_FALSE(late.locked);
    EXPECT_TRUE(late.unlocked);
    EXPECT_NEAR(late.position.x, a.position.x + 0.3f, 1e-6f);

    // pair_lock_position=0: every second eye moves with the body (the old behaviour).
    M::ComfortConfig off{};
    off.pair_lock = false;
    M::ComfortCamera cam2;
    cam2.Step({0.0f, Eye, 0.0f}, true, Start, ground, off);
    const M::ComfortOut b = cam2.Step({0.1f, Eye, 0.0f}, true, Start + Frame, ground, off);
    const M::ComfortOut second =
        cam2.Step({0.25f, Eye, 0.0f}, false, Start + 2 * Frame, ground, off);
    EXPECT_FALSE(second.locked);
    EXPECT_FALSE(second.unlocked);
    EXPECT_NEAR(second.position.x, b.position.x + 0.15f, 1e-6f);
}

TEST(ComfortCamera, DodgeDownStairs) {
    for (const bool stereo : {true, false}) {
        Scenario sc{};
        sc.seconds = 0.3 + QuickstepLeft.Length() + 1.5;
        sc.stereo = stereo;
        sc.root = [](double s) {
            const double d = QuickstepLeft.At(s - 0.3);
            return M::V3{static_cast<float>(d), Eye + Stairs(d), 0.0f};
        };
        sc.phase = [](double s) { return Dodge(7102, s - 0.3, 0.8); };
        const auto ticks = RunScenario(sc);
        const Metrics v = Measure(ticks, Y);
        EXPECT_LE(v.amax, 45.0f) << "stereo " << stereo;
        EXPECT_GE(v.settle, 0.0f);
        EXPECT_LE(v.settle, 0.40f) << "stereo " << stereo;
        EXPECT_LE(v.over, 0.01f) << "stereo " << stereo;
    }
}

TEST(ComfortCamera, WalkDownStairsAndStop) {
    for (const bool stereo : {true, false}) {
        Scenario sc{};
        sc.seconds = 4.0;
        sc.stereo = stereo;
        sc.root = [](double s) {
            const double d = std::min(1.5 * s, 4.5);
            return M::V3{static_cast<float>(d), Eye + Stairs(d), 0.0f};
        };
        sc.phase = [](double) { return Mode(M::Mode::Ground); };
        const auto ticks = RunScenario(sc);
        const Metrics v = Measure(ticks, Y);
        EXPECT_LE(v.amax, 30.0f) << "stereo " << stereo;
        EXPECT_GE(v.settle, 0.0f);
        EXPECT_LE(v.settle, 0.25f) << "stereo " << stereo;
    }
}

// Walking off a ledge of `height` m at 1.5 m/s: gravity while airborne, Fall (confirmed by the
// fall clips) until the landing, then Ground.
Scenario Ledge(float height, bool stereo) {
    Scenario sc{};
    sc.seconds = 4.0;
    sc.stereo = stereo;
    const double edge = 0.6 / 1.5;
    const double fall = std::sqrt(2.0 * height / 9.8);
    sc.root = [edge, height](double s) {
        const double drop = s > edge ? 0.5 * 9.8 * (s - edge) * (s - edge) : 0.0;
        return M::V3{static_cast<float>(1.5 * s),
                     Eye - static_cast<float>(std::min<double>(drop, height)), 0.0f};
    };
    sc.phase = [edge, fall](double s) {
        const bool air = s > edge && s < edge + fall;
        return air ? Mode(M::Mode::Fall, true) : Mode(M::Mode::Ground);
    };
    return sc;
}

TEST(ComfortCamera, FallsNeverRiseAndLandSmoothly) {
    for (const float height : {3.0f, 12.0f}) {
        for (const bool stereo : {true, false}) {
            const auto ticks = RunScenario(Ledge(height, stereo));
            float rise = 0.0f;
            u32 resets = 0;
            for (size_t i = 0; i < ticks.size(); ++i) {
                rise = std::max(rise, ticks[i].out.position.y - Eye);
                resets += (i > 0 && ticks[i].out.reset) ? 1 : 0;
            }
            EXPECT_LE(rise, 0.02f) << height;
            EXPECT_EQ(resets, 0u) << height;
            const Metrics v = Measure(ticks, Y);
            // The landing peak. Fall's 60 ms is held through the landing until the camera is
            // within 5 cm, which makes a 12 m landing a little firmer than the plan's simulation
            // (137 m/s^2 in stereo, which went to 120 ms at touchdown); mono samples it at 60 Hz.
            EXPECT_LE(v.amax, height < 5.0f ? 90.0f
                              : stereo      ? 155.0f
                                            : 180.0f)
                << height << " stereo " << stereo;
            EXPECT_GE(v.settle, 0.0f);
            EXPECT_LE(v.settle, 0.35f) << height << " stereo " << stereo;
        }
    }
    // A jump (the capsule rises 0.8 m) during a confirmed fall: the view never goes up with it.
    for (const bool stereo : {true, false}) {
        Scenario sc{};
        sc.seconds = 2.0;
        sc.stereo = stereo;
        sc.root = [](double s) {
            const double t = s - 0.3;
            const double up = t > 0.0 && t < 0.816 ? 4.0 * t - 4.9 * t * t : 0.0;
            return M::V3{static_cast<float>(2.0 * s), Eye + static_cast<float>(up), 0.0f};
        };
        sc.phase = [](double s) {
            const bool air = s > 0.3 && s < 0.3 + 0.816;
            return air ? Mode(M::Mode::Fall, true) : Mode(M::Mode::Ground);
        };
        const auto ticks = RunScenario(sc);
        float rise = 0.0f;
        for (const auto& t : ticks) {
            rise = std::max(rise, t.out.position.y - Eye);
        }
        EXPECT_LE(rise, 0.02f + 1e-5f) << "stereo " << stereo;
        EXPECT_GE(rise, 0.015f); // it does follow up to the margin
    }
}

TEST(ComfortCamera, TeleportSnapsLongFallDoesNot) {
    // A 5 m jump of the target (a lamp, a respawn) snaps on that tick, on either eye.
    for (const bool first : {true, false}) {
        M::ComfortCamera cam;
        const M::ComfortConfig cfg{};
        const M::Phase ground = Mode(M::Mode::Ground);
        s64 t = Start;
        for (int i = 0; i < 10; ++i, t += Frame) {
            cam.Step({0.0f, Eye, 0.0f}, i % 2 == 0, t, ground, cfg);
        }
        const M::ComfortOut out = cam.Step({5.0f, Eye, 0.0f}, first, t, ground, cfg);
        EXPECT_TRUE(out.reset);
        EXPECT_EQ(std::string(out.reset_reason), std::string("teleport"));
        EXPECT_NEAR(out.reset_m, 5.0f, 1e-4f);
        EXPECT_EQ(out.position.x, 5.0f);
    }
    // A 20 m/s fall for 2 s never snaps, in Fall mode or (no signals) in Ground mode.
    for (const M::Mode mode : {M::Mode::Fall, M::Mode::Ground}) {
        Scenario sc{};
        sc.seconds = 2.5;
        sc.root = [](double s) {
            return M::V3{0.0f, Eye - static_cast<float>(20.0 * std::min(s, 2.0)), 0.0f};
        };
        sc.phase = [mode](double) { return Mode(mode, mode == M::Mode::Fall); };
        const auto ticks = RunScenario(sc);
        for (size_t i = 1; i < ticks.size(); ++i) {
            EXPECT_FALSE(ticks[i].out.reset) << i;
        }
    }
    // The caller's reset (back from the game's camera) jumps at once.
    M::ComfortCamera cam;
    cam.Step({0.0f, Eye, 0.0f}, true, Start, Mode(M::Mode::Ground), {});
    const M::ComfortOut out =
        cam.Step({1.0f, Eye, 0.0f}, true, Start + Frame, Mode(M::Mode::Ground), {}, "inactive");
    EXPECT_TRUE(out.reset);
    EXPECT_EQ(std::string(out.reset_reason), std::string("inactive"));
    EXPECT_EQ(out.position.x, 1.0f);
}

TEST(ComfortCamera, ModeSwitchesAreContinuous) {
    // Walking down a ramp while the mode switches every 0.2 s.
    const M::Mode order[] = {M::Mode::Ground, M::Mode::Dodge, M::Mode::Fall,  M::Mode::Ground,
                             M::Mode::Ladder, M::Mode::Dodge, M::Mode::Ground};
    for (const bool stereo : {true, false}) {
        Scenario sc{};
        sc.seconds = 1.4;
        sc.stereo = stereo;
        sc.root = [](double s) {
            return M::V3{static_cast<float>(1.5 * s), Eye - static_cast<float>(0.8 * s), 0.0f};
        };
        sc.phase = [&order](double s) {
            const size_t i = std::min<size_t>(static_cast<size_t>(s / 0.2), 6);
            return Mode(order[i], order[i] == M::Mode::Fall);
        };
        const auto ticks = RunScenario(sc);
        // Vertical: the filter keeps its position and velocity across every switch; only the
        // time constant changes (a shorter one catches up, so the speed may exceed the body's
        // for a moment), never a jump. A jump of even 1 cm would show as > 300 m/s^2.
        const Metrics v = Measure(ticks, Y);
        EXPECT_LE(v.vmax, 2.5f * 0.8f) << "stereo " << stereo;
        EXPECT_LE(v.amax, 40.0f) << "stereo " << stereo;
        u32 changes = 0;
        for (const auto& t : ticks) {
            changes += t.out.mode_changed ? 1 : 0;
            EXPECT_FALSE(t.out.reset && t.t != Start);
        }
        EXPECT_GE(changes, 5u);
        // Horizontal: the modes never touch it (bit for bit the same as without switches).
        Scenario ground = sc;
        ground.phase = [](double) { return Mode(M::Mode::Ground); };
        const auto plain = RunScenario(ground);
        ASSERT_EQ(plain.size(), ticks.size());
        for (size_t i = 0; i < ticks.size(); ++i) {
            EXPECT_EQ(ticks[i].out.position.x, plain[i].out.position.x);
            EXPECT_EQ(ticks[i].out.position.z, plain[i].out.position.z);
        }
    }
}

TEST(ComfortCamera, OptionalDip) {
    const auto run = [](float dip_m, s32 dip_ms) {
        M::ComfortConfig cfg{};
        cfg.dip_m = dip_m;
        Scenario sc = FlatDodge(Roll, 7200, 1.0f, 0.0f, true);
        sc.phase = [dip_ms](double s) {
            M::Phase p = Dodge(7200, s - 0.3, 0.8);
            p.dodge.dip_ms = p.dodging ? dip_ms : 0;
            return p;
        };
        return RunScenario(sc, cfg);
    };
    const auto with = run(0.02f, 700);
    float deepest = 0.0f;
    double deepest_at = 0.0;
    for (const auto& t : with) {
        const double s = static_cast<double>(t.t - Start) * 1e-9;
        const float dip = t.out.position.y - Eye;
        if (dip < deepest) {
            deepest = dip;
            deepest_at = s - 0.3;
        }
        if (s < 0.3 || s > 0.3 + 0.7 + 0.02) {
            EXPECT_EQ(t.out.position.y, Eye) << s; // zero before and after
        }
        EXPECT_LE(dip, 0.0f);
    }
    EXPECT_NEAR(deepest, -0.02f, 0.0005f); // the roll's weight is 1
    EXPECT_NEAR(deepest_at, 0.35, 0.034);
    // Off: the same as a dodge without a dip, bit for bit.
    const auto off = run(0.0f, 700);
    const auto none = run(0.0f, 0);
    ASSERT_EQ(off.size(), none.size());
    for (size_t i = 0; i < off.size(); ++i) {
        EXPECT_EQ(std::memcmp(&off[i].out.position, &none[i].out.position, sizeof(M::V3)), 0);
    }
    // Quicksteps must never dip: the cue is for rolling, not sidestepping.
    M::ComfortConfig cfg{};
    cfg.dip_m = 0.02f;
    const auto quick = RunScenario(FlatDodge(Quickstep, 7100, 1.0f, 0.0f, true), cfg);
    float quick_deepest = 0.0f;
    for (const auto& t : quick) {
        quick_deepest = std::min(quick_deepest, t.out.position.y - Eye);
    }
    EXPECT_EQ(quick_deepest, 0.0f);
    // Every dodge dips, not only the first.
    Scenario twice{};
    twice.seconds = 3.0;
    twice.root = [](double) { return M::V3{0.0f, Eye, 0.0f}; };
    twice.phase = [](double s) {
        return s < 1.5 ? Dodge(7200, s - 0.3, 0.8) : Dodge(7200, s - 2.0, 0.8);
    };
    const auto both = RunScenario(twice, cfg);
    float first_dip = 0.0f, second_dip = 0.0f;
    for (const auto& t : both) {
        const double s = static_cast<double>(t.t - Start) * 1e-9;
        float& deepest_here = s < 1.5 ? first_dip : second_dip;
        deepest_here = std::min(deepest_here, t.out.position.y - Eye);
    }
    EXPECT_NEAR(first_dip, -0.02f, 0.0005f);
    EXPECT_NEAR(second_dip, -0.02f, 0.0005f);
}

TEST(ComfortCamera, WalkingLagFollowsTheTimeConstant) {
    // Walking at 1.5 m/s: the camera trails the body by a little under speed x time constant (the
    // continuous spring's lag; the discrete step at 30 / 60 Hz lags less), and not at all with
    // follow_smoothing_ms=0.
    for (const bool stereo : {true, false}) {
        const auto lag = [stereo](float follow) {
            Scenario sc{};
            sc.seconds = 2.0;
            sc.stereo = stereo;
            sc.root = [](double s) { return M::V3{static_cast<float>(1.5 * s), Eye, 0.0f}; };
            sc.phase = [](double) { return Mode(M::Mode::Ground); };
            M::ComfortConfig cfg{};
            cfg.follow_s = follow;
            const auto ticks = RunScenario(sc, cfg);
            const Tick& last = ticks[ticks.size() - (ticks.back().first ? 1 : 2)];
            return last.root.x - last.out.position.x;
        };
        const float rigid = lag(0.0f);
        const float normal = lag(0.025f);
        const float soft = lag(0.040f);
        EXPECT_EQ(rigid, 0.0f);
        EXPECT_GT(normal, 0.4f * 1.5f * 0.025f) << "stereo " << stereo;
        EXPECT_LE(normal, 1.5f * 0.025f) << "stereo " << stereo;
        EXPECT_GT(soft, 1.3f * normal) << "stereo " << stereo;
        EXPECT_LE(soft, 1.5f * 0.040f) << "stereo " << stereo;
    }
}

TEST(ComfortCamera, FallIsHeldThroughTheLanding) {
    // 3 m at 10 m/s, then the landing: the gate leaves Fall at once, the camera keeps Fall's time
    // constant until it is within 5 cm ...
    const auto landing = [](float fall_s) {
        Scenario sc{};
        sc.seconds = 1.5;
        sc.stereo = false;
        sc.root = [](double s) {
            const double drop = std::clamp(10.0 * (s - 0.2), 0.0, 3.0);
            return M::V3{0.0f, Eye - static_cast<float>(drop), 0.0f};
        };
        sc.phase = [](double s) {
            return s > 0.2 && s < 0.5 ? Mode(M::Mode::Fall, true) : Mode(M::Mode::Ground);
        };
        M::ComfortConfig cfg{};
        cfg.fall_s = fall_s;
        return RunScenario(sc, cfg);
    };
    const auto exit_of = [](const std::vector<Tick>& ticks, double* at) -> const Tick* {
        for (const auto& t : ticks) {
            const double s = static_cast<double>(t.t - Start) * 1e-9;
            if (s > 0.5 && t.out.mode_changed && t.out.mode_from == M::Mode::Fall) {
                *at = s - 0.5;
                return &t;
            }
        }
        return nullptr;
    };
    const auto fast = landing(0.060f);
    for (const auto& t : fast) {
        const double s = static_cast<double>(t.t - Start) * 1e-9;
        if (s > 0.5 && s < 0.52) {
            EXPECT_EQ(t.out.mode, M::Mode::Fall); // held: the camera is still well above
            EXPECT_EQ(t.out.t_eff, 0.060f);
        }
    }
    double at = 0.0;
    const Tick* exit = exit_of(fast, &at);
    ASSERT_TRUE(exit != nullptr);
    EXPECT_LT(exit->out.fall_lag_m, 0.05f);
    EXPECT_LT(at, 0.3);
    // ... or for 300 ms at most.
    const auto slow = landing(0.200f);
    exit = exit_of(slow, &at);
    ASSERT_TRUE(exit != nullptr);
    EXPECT_GE(exit->out.fall_lag_m, 0.05f);
    EXPECT_NEAR(at, 0.3, 0.02);
}

TEST(ComfortCamera, WorldScaleScalesEveryThreshold) {
    // The same stairs dodge at world_scale 2 with every position doubled: the camera doubles.
    const auto stairs_dodge = [](float scale) {
        Scenario sc{};
        sc.seconds = 2.5;
        sc.root = [scale](double s) {
            const double d = QuickstepLeft.At(s - 0.3);
            return M::V3{static_cast<float>(d) * scale, (Eye + Stairs(d)) * scale, 0.0f};
        };
        sc.phase = [](double s) { return Dodge(7102, s - 0.3, 0.8); };
        M::ComfortConfig cfg{};
        cfg.scale = scale;
        return RunScenario(sc, cfg);
    };
    const auto one = stairs_dodge(1.0f);
    const auto two = stairs_dodge(2.0f);
    ASSERT_EQ(one.size(), two.size());
    for (size_t i = 0; i < one.size(); ++i) {
        EXPECT_NEAR(two[i].out.position.x, 2.0f * one[i].out.position.x, 2e-4f);
        EXPECT_NEAR(two[i].out.position.y, 2.0f * one[i].out.position.y, 2e-4f);
        EXPECT_EQ(two[i].out.mode, one[i].out.mode);
    }
    // A 2.5-unit jump is a teleport at scale 1 (2.5 m), not at scale 2 (1.25 m).
    for (const float scale : {1.0f, 2.0f}) {
        M::ComfortCamera cam;
        M::ComfortConfig cfg{};
        cfg.scale = scale;
        cam.Step({0.0f, 0.0f, 0.0f}, true, Start, Mode(M::Mode::Ground), cfg);
        const M::ComfortOut out =
            cam.Step({2.5f, 0.0f, 0.0f}, true, Start + Frame, Mode(M::Mode::Ground), cfg);
        EXPECT_EQ(out.reset, scale == 1.0f) << scale;
    }
    // A confirmed fall's margin is 2 cm at either scale: the target rises 3 cm.
    for (const float scale : {1.0f, 2.0f}) {
        M::ComfortCamera cam;
        M::ComfortConfig cfg{};
        cfg.scale = scale;
        s64 t = Start;
        cam.Step({0.0f, Eye * scale, 0.0f}, true, t, Mode(M::Mode::Ground), cfg);
        M::ComfortOut out{};
        for (int i = 0; i < 60; ++i) {
            t += Frame;
            out = cam.Step({0.0f, (Eye + 0.03f) * scale, 0.0f}, true, t, Mode(M::Mode::Fall, true),
                           cfg);
        }
        EXPECT_NEAR(out.position.y, (Eye + 0.02f) * scale, 1e-3f * scale) << scale;
    }
}

TEST(ComfortCamera, StandingHalvesTheGroundTimeConstant) {
    M::ComfortCamera cam;
    const M::ComfortConfig cfg{};
    s64 t = Start;
    M::ComfortOut out{};
    for (int i = 0; i < 60; ++i, t += Frame) {
        out = cam.Step({0.0f, Eye, 0.0f}, true, t, Mode(M::Mode::Ground), cfg);
    }
    EXPECT_NEAR(out.t_eff, cfg.ground_s * cfg.stop_factor, 1e-3f);
    for (int i = 0; i < 60; ++i, t += Frame) {
        out = cam.Step({static_cast<float>(i) * 0.05f, Eye, 0.0f}, true, t, Mode(M::Mode::Ground),
                       cfg); // 3 m/s
    }
    EXPECT_NEAR(out.t_eff, cfg.ground_s, 1e-3f);
    // Entering Fall takes its time constant at once.
    out = cam.Step({3.0f, Eye - 0.1f, 0.0f}, true, t, Mode(M::Mode::Fall, true), cfg);
    EXPECT_EQ(out.t_eff, cfg.fall_s);
    EXPECT_TRUE(out.mode_changed);
}

TEST(ComfortCamera, TheFallClampStartsWhereTheFallIsConfirmed) {
    // A jump rising 0.6 m: "not landed" is an unconfirmed Fall 100 ms after the take-off while the
    // body still rises, and the jump-fall clip confirms the fall at the apex. The no-rise clamp
    // starts from the camera's height at the confirmation (not from where it was 100 ms after the
    // take-off, which would pull the view 0.4 m down at the apex at 200 m/s^2): never above that
    // height + 2 cm, and a soft landing back on the take-off level.
    for (const bool stereo : {true, false}) {
        const double v0 = std::sqrt(2.0 * 9.8 * 0.6), apex = v0 / 9.8, land = 2.0 * apex;
        Scenario sc{};
        sc.seconds = 2.0;
        sc.stereo = stereo;
        sc.root = [=](double s) {
            const double t = s - 0.3;
            const double up = t > 0.0 && t < land ? v0 * t - 4.9 * t * t : 0.0;
            return M::V3{static_cast<float>(2.0 * s), Eye + static_cast<float>(up), 0.0f};
        };
        sc.phase = [=](double s) {
            const double t = s - 0.3;
            if (t < 0.1 || t >= land) {
                return Mode(M::Mode::Ground);
            }
            return Mode(M::Mode::Fall, t > apex);
        };
        const auto ticks = RunScenario(sc);
        float confirmed_at = 0.0f, rise = -1e9f;
        bool confirmed = false;
        for (const auto& tick : ticks) {
            const double t = static_cast<double>(tick.t - Start) * 1e-9 - 0.3;
            if (!confirmed && tick.first && t > apex) {
                confirmed = true;
                confirmed_at = tick.out.takeoff;
            }
            if (confirmed && t < land) {
                rise = std::max(rise, tick.out.position.y - confirmed_at);
            }
        }
        const Metrics v = Measure(ticks, Y);
        ASSERT_TRUE(confirmed);
        EXPECT_GT(confirmed_at - Eye, 0.3f) << "the clamp starts high, near the apex";
        EXPECT_LE(rise, 0.021f) << "stereo " << stereo; // + the spring's last upward motion
        EXPECT_LE(v.amax, 40.0f) << "stereo " << stereo;
        EXPECT_GE(v.settle, 0.0f);
        EXPECT_LE(v.settle, 0.35f) << "stereo " << stereo;
    }
}

TEST(ComfortCamera, UnconfirmedFallTimeConstant) {
    // An unconfirmed Fall (the landed byte alone: stairs, a slope) blends its time constant over
    // 100 ms like any other mode; only a confirmed fall snaps to it. While a dodge is not landed
    // without a fall clip, the slower of fall_s and dodge_s.
    M::ComfortCamera cam;
    const M::ComfortConfig cfg{};
    s64 t = Start;
    M::ComfortOut out{};
    float x = 0.0f;
    const auto walk = [&](const M::Phase& phase, int ticks) {
        for (int i = 0; i < ticks; ++i, t += Frame) {
            x += 0.025f; // 1.5 m/s
            out = cam.Step({x, Eye, 0.0f}, true, t, phase, cfg);
        }
    };
    walk(Mode(M::Mode::Ground), 60);
    EXPECT_NEAR(out.t_eff, cfg.ground_s, 1e-3f);
    walk(Mode(M::Mode::Fall, false), 1);
    EXPECT_EQ(out.mode, M::Mode::Fall);
    const float k = 1.0f - std::exp(-static_cast<float>(Frame) * 1e-9f / M::ComfortCamera::BlendS);
    EXPECT_NEAR(out.t_eff, cfg.ground_s + (cfg.fall_s - cfg.ground_s) * k, 1e-4f);
    walk(Mode(M::Mode::Fall, false), 30);
    EXPECT_NEAR(out.t_eff, cfg.fall_s, 1e-3f);
    M::Phase rolling = Mode(M::Mode::Fall, false);
    rolling.dodging = true;
    walk(rolling, 30);
    EXPECT_NEAR(out.t_eff, std::max(cfg.fall_s, cfg.dodge_s), 1e-3f);
    rolling.fall_confirmed = true; // a roll off a ledge: the fall's own
    walk(rolling, 30);
    EXPECT_NEAR(out.t_eff, cfg.fall_s, 1e-3f);
    // A confirmed fall entered from Ground snaps.
    walk(Mode(M::Mode::Ground), 60);
    walk(Mode(M::Mode::Fall, true), 1);
    EXPECT_EQ(out.t_eff, cfg.fall_s);
}

TEST(ComfortCamera, NonFiniteTargetsKeepTheLastPosition) {
    // A non-finite target (a bad position read) never enters the filter: that tick returns the
    // last position, and the next finite target starts over from it (a reset), on either eye.
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();
    for (const bool first : {true, false}) {
        M::ComfortCamera cam;
        const M::ComfortConfig cfg{};
        s64 t = Start;
        M::ComfortOut out{};
        int i = 0;
        for (; i < 20; ++i, t += Frame) {
            out = cam.Step({0.02f * static_cast<float>(i), Eye, 0.0f}, i % 2 == 0, t,
                           Mode(M::Mode::Ground), cfg);
        }
        const M::V3 last = out.position;
        for (const M::V3 bad : {M::V3{nan, Eye, 0.0f}, M::V3{0.4f, inf, 0.0f}}) {
            out = cam.Step(bad, first, t, Mode(M::Mode::Ground), cfg);
            t += Frame;
            EXPECT_TRUE(out.rejected);
            EXPECT_EQ(out.position.x, last.x);
            EXPECT_EQ(out.position.y, last.y);
        }
        const M::V3 good{0.5f, Eye - 0.1f, 0.0f};
        out = cam.Step(good, first, t, Mode(M::Mode::Ground), cfg);
        t += Frame;
        EXPECT_TRUE(out.reset);
        EXPECT_EQ(std::string(out.reset_reason), std::string("non-finite"));
        EXPECT_EQ(out.position.x, good.x);
        for (i = 0; i < 30; ++i, t += Frame) {
            out = cam.Step({0.5f + 0.02f * static_cast<float>(i), Eye - 0.1f, 0.0f}, i % 2 == 0, t,
                           Mode(M::Mode::Ground), cfg);
            EXPECT_FALSE(out.reset);
            EXPECT_TRUE(std::isfinite(out.position.x) && std::isfinite(out.position.y));
        }
    }
    // The caller's fallback is the body's physics position plus the eye height from the main
    // player's last update, read apart from the follow camera's pivot the target comes from: the
    // game derives the pivot from that position, so a bad pivot makes the target non-finite while
    // the fallback stays good.
    const auto finite = [](const M::V3& p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    };
    const auto same = [](const M::V3& a, const M::V3& b) {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };
    const M::V3 body{3.0f, 1.4f, -2.0f};
    const M::Fallback fresh{body, Start - 10 * Ms};
    // Before the first finite target: the body's position (never the origin); with nothing known
    // at all, a finite position all the same (the origin, flagged stale); with only an old body
    // position, that one (stale). The first finite target is "init".
    {
        M::ComfortCamera cam;
        M::ComfortOut out = cam.Step({nan, Eye, 0.0f}, true, Start, Mode(M::Mode::Ground), {});
        EXPECT_TRUE(out.rejected && out.stale && !out.fallback);
        EXPECT_TRUE(finite(out.position));
        const M::Fallback old{body, Start - 3000 * Ms};
        out = cam.Step({nan, Eye, 0.0f}, true, Start, Mode(M::Mode::Ground), {}, nullptr, nullptr,
                       &old);
        EXPECT_TRUE(out.rejected && out.fallback && out.stale);
        EXPECT_TRUE(same(out.position, body));
        out = cam.Step({nan, Eye, 0.0f}, true, Start, Mode(M::Mode::Ground), {}, nullptr, nullptr,
                       &fresh);
        EXPECT_TRUE(out.rejected && out.fallback && !out.stale);
        EXPECT_TRUE(same(out.position, body));
        out = cam.Step({1.0f, Eye, 0.0f}, true, Start + Frame, Mode(M::Mode::Ground), {});
        EXPECT_EQ(std::string(out.reset_reason), std::string("init"));
    }
    // A reset asked for (the camera was not driven for 8 s: a cutscene, a loading screen; the body
    // is elsewhere now) and a non-finite target on that tick: neither the output from before the
    // gap nor the 8 s old target, the body's position now. Without a recent one: the newest
    // position known, flagged stale (the body's from before the gap is older than the last target:
    // that target). The next finite target resets.
    M::ComfortCamera cam;
    M::ComfortOut out{};
    for (int i = 0; i < 10; ++i) {
        out = cam.Step({100.0f, Eye, 40.0f}, true, Start + i * Frame, Mode(M::Mode::Ground), {});
    }
    s64 t = Start + 8000 * Ms;
    const M::Fallback now_body{{-30.0f, 2.6f, 12.0f}, t - 12 * Ms};
    out = cam.Step({nan, Eye, nan}, true, t, Mode(M::Mode::Ground), {}, "inactive", nullptr,
                   &now_body);
    EXPECT_TRUE(out.rejected && out.fallback && !out.stale);
    EXPECT_TRUE(same(out.position, now_body.position));
    const M::Fallback before_gap{{90.0f, 2.6f, 40.0f}, Start};
    out = cam.Step({nan, Eye, nan}, true, t, Mode(M::Mode::Ground), {}, "inactive", nullptr,
                   &before_gap);
    EXPECT_TRUE(out.rejected && !out.fallback && out.stale);
    EXPECT_EQ(out.position.x, 100.0f);
    const M::Fallback during_gap{{-20.0f, 2.6f, 12.0f}, t - 4000 * Ms};
    out = cam.Step({nan, Eye, nan}, true, t, Mode(M::Mode::Ground), {}, "inactive", nullptr,
                   &during_gap);
    EXPECT_TRUE(out.rejected && out.fallback && out.stale);
    EXPECT_EQ(out.position.x, -20.0f);
    out = cam.Step({-5.0f, Eye, 0.0f}, true, t + Frame, Mode(M::Mode::Ground), {});
    EXPECT_TRUE(out.reset);
    EXPECT_EQ(out.position.x, -5.0f);
    // A long run of bad reads, the body walking on (its position read every update): the last
    // output for 250 ms, then the body's position; never non-finite, never the origin.
    t += 2 * Frame;
    int kept = 0, fell_back = 0;
    for (int i = 0; i < 30; ++i, t += Frame) {
        const M::Fallback walking{{-5.0f + 0.07f * static_cast<float>(i), 2.6f, 0.0f}, t - 5 * Ms};
        out = cam.Step({nan, Eye, 0.0f}, i % 2 == 0, t, Mode(M::Mode::Ground), {}, nullptr, nullptr,
                       &walking);
        EXPECT_TRUE(finite(out.position));
        EXPECT_FALSE(out.stale);
        kept += out.fallback ? 0 : 1;
        fell_back += out.fallback ? 1 : 0;
        if (out.fallback) {
            EXPECT_TRUE(same(out.position, walking.position));
        } else {
            EXPECT_EQ(out.position.x, -5.0f);
        }
    }
    EXPECT_GE(kept, 14);
    EXPECT_LE(kept, 16);
    EXPECT_EQ(kept + fell_back, 30);
}

TEST(ComfortCamera, SecondEyeFollowsTheBodysLockDecision) {
    // The VR body decides a second eye's pair lock at its pose copy (it places the hands for that
    // eye there, before the camera tick); the camera follows that decision when it belongs to this
    // pair, else its own 1.5-frame window decides.
    const M::Phase ground = Mode(M::Mode::Ground);
    const M::ComfortConfig cfg{}; // pair_max_ns 25 ms
    const auto pair = [&](s64 second_at, const M::LockDecision* lock) {
        M::ComfortCamera cam;
        cam.Step({0.0f, Eye, 0.0f}, true, Start, ground, cfg);
        const M::ComfortOut first = cam.Step({0.1f, Eye, 0.0f}, true, Start + Frame, ground, cfg);
        const M::ComfortOut second = cam.Step({0.4f, Eye, 0.0f}, false, Start + Frame + second_at,
                                              ground, cfg, nullptr, lock);
        return std::pair{first, second};
    };
    const s64 first_tick = Start + Frame;
    // 28 ms after the first eye: past the camera's own window, but the body (its copy 6 ms before
    // this tick) decided "locked": locked, bit for bit.
    M::LockDecision locked{true, first_tick + 100'000, first_tick + 22 * Ms, true};
    auto [a, b] = pair(28 * Ms, &locked);
    EXPECT_TRUE(b.decided);
    EXPECT_TRUE(b.locked);
    EXPECT_EQ(std::memcmp(&a.position, &b.position, sizeof(M::V3)), 0);
    // 20 ms after it (inside the window), the body decided "not locked": moves with the body.
    M::LockDecision unlocked{true, first_tick + 100'000, first_tick + 14 * Ms, false};
    std::tie(a, b) = pair(20 * Ms, &unlocked);
    EXPECT_TRUE(b.decided);
    EXPECT_FALSE(b.locked);
    EXPECT_TRUE(b.unlocked);
    EXPECT_NEAR(b.position.x, a.position.x + 0.3f, 1e-6f);
    // Not this pair's decision (made before this pair's first eye, for an older first eye, or
    // after this tick): the camera's own window (20 ms: locked; 28 ms: not).
    for (const M::LockDecision other :
         {M::LockDecision{true, first_tick - Frame, first_tick - 10 * Ms, false},
          M::LockDecision{true, first_tick - Frame, first_tick + 14 * Ms, false},
          M::LockDecision{true, first_tick + 100'000, first_tick + 30 * Ms, false},
          M::LockDecision{}}) {
        std::tie(a, b) = pair(20 * Ms, &other);
        EXPECT_FALSE(b.decided);
        EXPECT_TRUE(b.locked);
        M::LockDecision late_lock = other;
        late_lock.locked = true;
        std::tie(a, b) = pair(28 * Ms, &late_lock);
        EXPECT_FALSE(b.decided);
        EXPECT_FALSE(b.locked);
    }
}

// ---- Per-dodge statistics --------------------------------------------------------------------

TEST(DodgeStats, ReportsAFlatQuickstep) {
    const auto ticks = RunScenario(FlatDodge(Quickstep, 7100, 1.0f, 0.0f, true));
    M::DodgeStats stats;
    stats.Begin(ticks.front().t);
    for (const auto& t : ticks) {
        if (t.first) {
            stats.Add(t.t, t.root, t.out.position);
        } else {
            ++stats.pairs;
        }
    }
    stats.End(ticks.back().t - 300 * Ms);
    EXPECT_TRUE(stats.Due(ticks.back().t));
    const M::DodgeReport r = stats.Finish(1.0f);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.root_dist, static_cast<float>(Quickstep.Distance(10.0)), 0.01f);
    EXPECT_NEAR(r.dist_ratio, 1.0f, 0.03f);
    EXPECT_LE(r.dir_err_deg, 2.0f);
    EXPECT_LE(r.h_lag_max, 0.25f);
    EXPECT_LE(r.h_over, 0.005f);
    EXPECT_GE(r.h_settle_ms, 0.0f);
    EXPECT_LE(r.h_settle_ms, 50.0f);
    EXPECT_EQ(r.v_root_range, 0.0f);
    EXPECT_EQ(r.v_cam_range, 0.0f);
    EXPECT_FALSE(stats.Active());
}

} // namespace

TEST(ComfortCamera, ActualJumpLiftAndTwelveCmRollReachCameraAtEveryRate) {
    for(int hz:{30,60,72,80,90,120}) for(int clip:{7200,7250,7800}) {
        M::ComfortCamera camera;
        M::ComfortConfig cfg{}; cfg.jump_height_scale=1; cfg.dip_m=.13f;
        float peak=0, previous=Eye;
        for(int i=0;i<hz*3;++i) {
            const auto now=Start+s64(i)*1'000'000'000/hz;
            const float t=float(i)/hz-.3f;
            cfg.jump_lift=t>0 && t<.8f ? .6f*std::sin(3.14159265f*t/.8f)-.1f : 0.f;
            const auto out=camera.Step({0,Eye,0},true,now,Dodge(clip,t,1.6),cfg);
            const float offset=out.position.y-Eye;
            if(clip==7800) { EXPECT_GE(offset,-1e-6f); peak=std::max(peak,offset); }
            else { EXPECT_LE(offset,1e-6f); peak=std::min(peak,offset); }
            EXPECT_LT(std::abs(out.position.y-previous),clip==7800?.08f:.025f);
            previous=out.position.y;
        }
        EXPECT_NEAR(peak,clip==7800?.5f:-.13f,clip==7800?.012f:.002f)<<hz<<" clip "<<clip;
        EXPECT_FLOAT_EQ(previous,Eye);
    }
}
TEST(ComfortCamera, JumpLiftNeverAppliesToOtherAnimationsOrWhenDisabled) {
    for(int clip:{7000,7100,7150,7400,7800}) {
        M::ComfortCamera camera; M::ComfortConfig cfg{};
        cfg.jump_lift=.5f; cfg.jump_height_scale=clip==7800?0.f:1.f;
        for(int i=0;i<120;++i) {
            auto out=camera.Step({0,Eye,0},true,Start+i*Frame,Dodge(clip,i/60.-.3,1.6),cfg);
            EXPECT_FLOAT_EQ(out.position.y,Eye)<<clip;
        }
    }
}

TEST(ComfortCamera, StandingOnLowerStepSettlesToBodyHeightForBothEyes) {
    for (const int hz : {60, 90, 120}) {
        M::ComfortCamera camera;
        M::ComfortConfig cfg{};
        M::Phase ground{};
        camera.Step({0, Eye, 0}, true, Start, ground, cfg);
        const float body_eye = Eye - .24f; // a lower stair, independent of an elevated pivot
        float previous = Eye;
        for (int i = 1; i <= hz; ++i) {
            const s64 now = Start + static_cast<s64>(i * 1e9 / hz);
            const auto first = camera.Step({0, body_eye, 0}, true, now, ground, cfg);
            const auto second = camera.Step({0, body_eye, 0}, false, now + 1, ground, cfg);
            EXPECT_LE(first.position.y, previous + 1e-6f);
            EXPECT_GE(first.position.y, body_eye - 1e-6f);
            EXPECT_FLOAT_EQ(first.position.y, second.position.y);
            previous = first.position.y;
        }
        EXPECT_NEAR(previous, body_eye, .001f);
    }
}
