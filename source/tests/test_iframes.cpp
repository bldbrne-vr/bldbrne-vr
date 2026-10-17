// SPDX-License-Identifier: GPL-2.0-or-later

// Dodge i-frame extension (core/vr/iframe_extend.h) against a model of the game's update: the
// reset at the end of PlayerIns::Update, TAE event 8 in the behaviour step (start <= t1 and
// end > t0, t0 = -1 on a clip's first update), then the hook. "Protected" updates are the ones
// whose site A (inside the next Update, before the reset) sees the bit, as an attack would.

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/iframe_extend.h"

namespace {

namespace I = VR::Iframes;

constexpr float Dt60 = 1.0f / 60.0f;
constexpr float Dt30 = 1.0f / 30.0f;

struct Clip {
    float f8_start{-1.0f}; // < 0: no F8 window
    float f8_end{-1.0f};
    float sp392_end{-1.0f}; // < 0: no SpEffect 392
    float length{1.0f};
    bool dodge{true}; // a dodge clip (7000-7799) in the TimeAct ring
};

const Clip Roll{0.0f, 0.367f, 0.800f, 2.10f};        // 7200
const Clip Quickstep{0.0f, 0.367f, 0.733f, 2.10f};   // 7100
const Clip QuickstepOhb{0.0f, 0.267f, 0.567f, 1.9f}; // 7150
const Clip Backstep{-1.0f, -1.0f, -1.0f, 2.07f};     // 7000: SpEffect 393 only
const Clip Jump{0.767f, 1.167f, -1.0f, 2.60f, false};   // 7800
const Clip Knockdown{0.0f, 1.500f, -1.0f, 3.0f, false}; // a knockdown with a long F8 window
// A dodge whose F8 window opens 3 updates (60 fps) into the clip: a cut in its first updates
// arrives before the window (the swing's thread ahead of the hook's).
const Clip LateRoll{0.05f, 0.367f, 0.800f, 2.10f};

struct Game {
    explicit Game(float extra_s) : ext(extra_s) {}

    I::Extender ext;
    std::uint32_t bits{0};
    bool behavior{true};
    bool grounded{true};
    bool alive{true};
    bool interrupt_next{false};
    bool cut_next{false}; // an attack during the dodge (dodge_attack_iframes=0)
    int cut_starts{0};
    bool ring{true}; // the hook reads the TimeAct ring (false: only SpEffect 392 tells a dodge)
    const Clip* clip{nullptr};
    float clip_t{0.0f};
    bool first{false};
    // Results.
    int asserts{0};
    int clears{0};
    int windows{0};
    int extended{0};
    std::vector<I::End> ends;
    std::vector<int> runs; // protected runs seen at site A, in updates
    int run{0};
    std::vector<bool> asserted; // per update

    void Start(const Clip& c) {
        clip = &c;
        clip_t = 0.0f;
        first = true;
    }

    void Update(float dt) {
        // Site A, inside PlayerIns::Update: what an attack would see.
        if ((bits & 2u) != 0) {
            ++run;
        } else if (run > 0) {
            runs.push_back(run);
            run = 0;
        }
        // The reset at the end of PlayerIns::Update (skipped while the behaviour is idle).
        if (behavior) {
            bits = 0;
        }
        // The behaviour step: the clip advances and its F8 sets the bit.
        bool sp392 = false;
        bool dodge_clip = false;
        bool dodge_start = false;
        if (clip != nullptr && behavior) {
            dodge_clip = clip->dodge;
            dodge_start = clip->dodge && first;
            const float t0 = first ? -1.0f : clip_t;
            clip_t += dt;
            const float t1 = clip_t;
            first = false;
            if (clip->f8_start >= 0.0f && clip->f8_start <= t1 && clip->f8_end > t0) {
                bits |= 2u;
            }
            if (clip_t >= clip->length) {
                clip = nullptr;
            }
        }
        if (clip != nullptr) {
            sp392 = clip->sp392_end >= 0.0f && clip_t <= clip->sp392_end;
        }
        // The hook at vt+0xE8.
        I::Input in{};
        in.tae = (bits & 2u) != 0;
        in.reset_ran = behavior;
        in.dodge = sp392;
        in.grounded = grounded;
        in.alive = alive;
        in.interrupted = interrupt_next;
        interrupt_next = false;
        in.cut = cut_next;
        cut_next = false;
        // As the hook: a dodge clip stepped (the ring) or the dodge SpEffect.
        in.dodge_clip = (ring && dodge_clip) || sp392;
        in.dodge_start = ring && dodge_start;
        in.dt = dt;
        const I::Output o = ext.Step(in);
        if (o.cut_bit) {
            bits &= ~2u;
        }
        if (o.cut_start) {
            ++cut_starts;
        }
        if (o.assert_bit) {
            bits |= 2u;
            ++asserts;
        }
        if (o.clear_bit) {
            bits &= ~2u;
            ++clears;
        }
        if (o.window_start) {
            ++windows;
        }
        if (o.window_end && o.extended) {
            ++extended;
        }
        if ((o.window_end && !o.extended) || o.extension_end) {
            ends.push_back(o.end);
        }
        asserted.push_back(o.assert_bit);
    }

    void Run(float dt, float seconds) {
        const int n = static_cast<int>(std::lround(seconds / dt));
        for (int i = 0; i < n; ++i) {
            Update(dt);
        }
    }
};

// The vanilla protected run of a clip at a frame rate.
int VanillaRun(const Clip& c, float dt) {
    Game g(0.0f);
    g.Run(dt, 0.2f);
    g.Start(c);
    g.Run(dt, 3.0f);
    return g.runs.empty() ? 0 : g.runs.front();
}

TEST(IframeExtend, RollAt60GetsFourUpdates) {
    const int vanilla = VanillaRun(Roll, Dt60);
    EXPECT_GE(vanilla, 22);
    EXPECT_LE(vanilla, 23);
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Roll);
    g.Run(Dt60, 3.0f);
    EXPECT_EQ(g.asserts, 4);
    EXPECT_EQ(g.windows, 1);
    EXPECT_EQ(g.extended, 1);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0], vanilla + 4);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Timer);
    // The asserted updates follow the window without a gap.
    int first = -1;
    for (int i = 0; i < static_cast<int>(g.asserted.size()); ++i) {
        if (g.asserted[i]) {
            first = first < 0 ? i : first;
        }
    }
    ASSERT_GT(first, 0);
    for (int i = first; i < first + 4; ++i) {
        EXPECT_TRUE(g.asserted[i]);
    }
    EXPECT_FALSE(g.asserted[first + 4]);
}

TEST(IframeExtend, RollAt30GetsTwoUpdates) {
    const int vanilla = VanillaRun(Roll, Dt30);
    Game g(2.0f * I::GameFrame);
    g.Run(Dt30, 0.2f);
    g.Start(Roll);
    g.Run(Dt30, 3.0f);
    EXPECT_EQ(g.asserts, 2);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0], vanilla + 2);
}

TEST(IframeExtend, QuickstepsAndOhb) {
    for (const Clip* c : {&Quickstep, &QuickstepOhb}) {
        const int vanilla = VanillaRun(*c, Dt60);
        Game g(2.0f * I::GameFrame);
        g.Run(Dt60, 0.2f);
        g.Start(*c);
        g.Run(Dt60, 3.0f);
        EXPECT_EQ(g.asserts, 4);
        ASSERT_EQ(g.runs.size(), 1u);
        EXPECT_EQ(g.runs[0], vanilla + 4);
    }
}

TEST(IframeExtend, BackstepGetsNothing) {
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Backstep);
    g.Run(Dt60, 3.0f);
    EXPECT_EQ(g.asserts, 0);
    EXPECT_EQ(g.windows, 0);
    EXPECT_TRUE(g.runs.empty());
}

TEST(IframeExtend, JumpAndKnockdownStayVanilla) {
    for (const Clip* c : {&Jump, &Knockdown}) {
        const int vanilla = VanillaRun(*c, Dt60);
        Game g(2.0f * I::GameFrame);
        g.Run(Dt60, 0.2f);
        g.Start(*c);
        g.Run(Dt60, 3.5f);
        EXPECT_EQ(g.asserts, 0);
        EXPECT_EQ(g.windows, 1);
        ASSERT_EQ(g.runs.size(), 1u);
        EXPECT_EQ(g.runs[0], vanilla);
        ASSERT_EQ(g.ends.size(), 1u);
        EXPECT_EQ(g.ends[0], I::End::NoDodge);
    }
}

TEST(IframeExtend, AirborneAtWindowEndGetsNothing) {
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Roll);
    g.Run(Dt60, 0.30f);
    g.grounded = false; // rolled off a ledge
    g.Run(Dt60, 2.0f);
    EXPECT_EQ(g.asserts, 0);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Airborne);
}

TEST(IframeExtend, AirborneDuringExtensionStops) {
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Roll);
    // The window covers 22-23 updates; two extension updates, then off a ledge.
    while (g.asserts < 2) {
        g.Update(Dt60);
    }
    g.grounded = false;
    g.Run(Dt60, 2.0f);
    EXPECT_EQ(g.asserts, 2);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Airborne);
}

TEST(IframeExtend, DeadOrInterruptedStops) {
    {
        Game g(2.0f * I::GameFrame);
        g.Start(Roll);
        while (g.asserts < 1) {
            g.Update(Dt60);
        }
        g.alive = false;
        g.Run(Dt60, 1.0f);
        EXPECT_EQ(g.asserts, 1);
        ASSERT_EQ(g.ends.size(), 1u);
        EXPECT_EQ(g.ends[0], I::End::Dead);
    }
    {
        Game g(2.0f * I::GameFrame);
        g.Start(Roll);
        while (g.asserts < 1) {
            g.Update(Dt60);
        }
        g.interrupt_next = true; // a damage clip started
        g.Run(Dt60, 1.0f);
        EXPECT_EQ(g.asserts, 1);
        ASSERT_EQ(g.ends.size(), 1u);
        EXPECT_EQ(g.ends[0], I::End::Interrupted);
    }
}

TEST(IframeExtend, ChainedDodgesEachGetTheirOwn) {
    const int vanilla = VanillaRun(Roll, Dt60);
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Roll);
    g.Run(Dt60, 0.6f); // the next dodge after the cancel window
    g.Start(Roll);
    g.Run(Dt60, 3.0f);
    EXPECT_EQ(g.windows, 2);
    EXPECT_EQ(g.asserts, 8);
    ASSERT_EQ(g.runs.size(), 2u);
    EXPECT_EQ(g.runs[0], vanilla + 4);
    EXPECT_EQ(g.runs[1], vanilla + 4);
}

TEST(IframeExtend, NewWindowDuringExtension) {
    Game g(2.0f * I::GameFrame);
    g.Start(Roll);
    while (g.asserts < 2) {
        g.Update(Dt60);
    }
    g.Start(Roll); // (not possible in the game: cancels open 0.133 s after the window)
    g.Run(Dt60, 3.0f);
    EXPECT_EQ(g.windows, 2);
    EXPECT_EQ(g.asserts, 2 + 4);
    ASSERT_EQ(g.ends.size(), 2u);
    EXPECT_EQ(g.ends[0], I::End::NewWindow);
    EXPECT_EQ(g.ends[1], I::End::Timer);
    ASSERT_EQ(g.runs.size(), 1u); // one continuous protected run
}

TEST(IframeExtend, StaleBitWithIdleBehaviourIsCleared) {
    Game g(2.0f * I::GameFrame);
    g.Start(Roll);
    while (g.asserts < 1) {
        g.Update(Dt60);
    }
    g.behavior = false; // no reset, no TAE: our bit would stay forever
    g.Update(Dt60);
    EXPECT_EQ(g.clears, 1);
    EXPECT_EQ(g.bits & 2u, 0u);
    g.Run(Dt60, 1.0f);
    EXPECT_EQ(g.asserts, 1);
    EXPECT_EQ(g.clears, 1);
    EXPECT_EQ(g.bits & 2u, 0u);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Stale);
}

TEST(IframeExtend, ZeroExtraIsVanilla) {
    const int vanilla = VanillaRun(Roll, Dt60);
    Game g(0.0f);
    g.Start(Roll);
    g.Run(Dt60, 3.0f);
    EXPECT_EQ(g.asserts, 0);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0], vanilla);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Disabled);
}

TEST(IframeExtend, JitteryFrameTimes) {
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int trial = 0; trial < 200; ++trial) {
        const float extra = static_cast<float>(1 + trial % 4) * I::GameFrame;
        Game g(extra);
        g.Start(Roll);
        float max_dt = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const float dt = 1.0f / 75.0f + next() * (1.0f / 40.0f - 1.0f / 75.0f);
            const bool before = g.ext.Extending();
            g.Update(dt);
            if (before || g.ext.Extending()) {
                max_dt = std::max(max_dt, dt);
            }
        }
        EXPECT_GT(g.asserts, 0);
        EXPECT_NEAR(g.ext.ExtensionTime(), extra, 0.5f * max_dt + 1e-4f);
    }
}

TEST(IframeExtend, ClipClasses) {
    EXPECT_TRUE(I::IsDodgeClip(7000));
    EXPECT_TRUE(I::IsDodgeClip(7200));
    EXPECT_TRUE(I::IsDodgeClip(2007105));
    EXPECT_FALSE(I::IsDodgeClip(7800));
    EXPECT_FALSE(I::IsDodgeClip(100007200));
    EXPECT_TRUE(I::IsInterruptClip(7800));
    EXPECT_TRUE(I::IsInterruptClip(20000));
    EXPECT_TRUE(I::IsInterruptClip(24100));
    EXPECT_FALSE(I::IsInterruptClip(30000));
    EXPECT_FALSE(I::IsInterruptClip(7100));
}

// Attacks through dodges (dodge_attack, dodge_attack_iframes=0): a swing 0.1 s into a roll ends
// its invulnerability that update (the F8 window's own bit is cleared too) and no extension
// follows; one during the extension ends it; the next roll has its own full window again.
TEST(IframeExtend, AnAttackEndsTheDodgesIframes) {
    const int vanilla = VanillaRun(Roll, Dt60);
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(Roll);
    g.Run(Dt60, 0.1f); // 6 protected updates
    g.cut_next = true;
    g.Run(Dt60, 2.9f);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0], 6);
    EXPECT_EQ(g.asserts, 0);
    EXPECT_EQ(g.extended, 0);
    ASSERT_FALSE(g.ends.empty());
    EXPECT_EQ(g.ends.back(), I::End::Attack);
    EXPECT_FALSE(g.ext.Cut()); // over with its clip
    // The next roll: its own window and extension.
    g.Start(Roll);
    g.Run(Dt60, 3.0f);
    ASSERT_EQ(g.runs.size(), 2u);
    EXPECT_EQ(g.runs[1], vanilla + 4);
    EXPECT_FALSE(g.ext.Cut());

    // During the extension (the window's 22-23 updates, then 4 more), after two of them: it ends.
    Game e(2.0f * I::GameFrame);
    e.Run(Dt60, 0.2f);
    e.Start(Roll);
    e.Run(Dt60, static_cast<float>(vanilla + 2) * Dt60);
    e.cut_next = true;
    e.Run(Dt60, 2.5f);
    ASSERT_EQ(e.runs.size(), 1u);
    EXPECT_EQ(e.runs[0], vanilla + 2); // the window and two extension updates
    EXPECT_EQ(e.ends.back(), I::End::Attack);

    // A swing still running as the next roll starts (the request in its first update): that roll
    // has no i-frames at all, and the cut is a new one (cut_start).
    Game c(2.0f * I::GameFrame);
    c.Run(Dt60, 0.2f);
    c.Start(Roll);
    c.cut_next = true;
    c.Run(Dt60, 0.5f);
    c.Start(Roll);
    c.cut_next = true;
    c.Run(Dt60, 2.5f);
    EXPECT_TRUE(c.runs.empty());
    EXPECT_EQ(c.cut_starts, 2);

    // A backstep's attack (no window): nothing to end, the next roll is whole.
    Game b(2.0f * I::GameFrame);
    b.Run(Dt60, 0.2f);
    b.Start(Backstep);
    b.Run(Dt60, 0.1f);
    b.cut_next = true;
    b.Run(Dt60, 2.5f);
    EXPECT_TRUE(b.runs.empty());
    b.Start(Roll);
    b.Run(Dt60, 3.0f);
    ASSERT_EQ(b.runs.size(), 1u);
    EXPECT_EQ(b.runs[0], vanilla + 4);
}

// The cut race (the swing on Game:Main, the hook on CSChrThread5): a cut that reaches the hook
// before its dodge's i-frame window opens waits for it (pending) instead of being dropped when the
// window starts; it lasts as long as its dodge clip, and the next dodge clip has its own window.
TEST(IframeExtend, ACutBeforeTheWindowWaitsForIt) {
    const int vanilla = VanillaRun(Roll, Dt60);
    // In the clip's first update, 2 updates before its F8: no i-frames at all, no extension.
    Game g(2.0f * I::GameFrame);
    g.Run(Dt60, 0.2f);
    g.Start(LateRoll);
    g.cut_next = true;
    g.Update(Dt60);
    EXPECT_TRUE(g.ext.CutPending());
    EXPECT_FALSE(g.ext.InWindow());
    g.Run(Dt60, 0.2f);
    EXPECT_TRUE(g.ext.Cut());
    g.Run(Dt60, 2.8f);
    EXPECT_TRUE(g.runs.empty());
    EXPECT_EQ(g.windows, 1);
    EXPECT_EQ(g.asserts, 0);
    ASSERT_EQ(g.ends.size(), 1u);
    EXPECT_EQ(g.ends[0], I::End::Attack);
    EXPECT_FALSE(g.ext.Cut()); // its clip ended
    EXPECT_EQ(g.cut_starts, 1);

    // One update before its clip shows in the ring (the gate a step ahead): it binds to it.
    Game a(2.0f * I::GameFrame);
    a.Run(Dt60, 0.2f);
    a.cut_next = true;
    a.Update(Dt60);
    a.Start(Roll);
    a.Run(Dt60, 3.0f);
    EXPECT_TRUE(a.runs.empty());
    EXPECT_EQ(a.asserts, 0);

    // A cut with no dodge clip then or in the next update: over (the next roll is whole).
    Game n(2.0f * I::GameFrame);
    n.Run(Dt60, 0.2f);
    n.cut_next = true;
    n.Run(Dt60, 2.0f * Dt60);
    EXPECT_FALSE(n.ext.Cut());
    n.Start(Roll);
    n.Run(Dt60, 3.0f);
    ASSERT_EQ(n.runs.size(), 1u);
    EXPECT_EQ(n.runs[0], vanilla + 4);

    // Past its window (in the roll's recovery): pending until the clip ends or the next dodge
    // clip starts; a chained roll has its own window and extension.
    Game c(2.0f * I::GameFrame);
    c.Run(Dt60, 0.2f);
    c.Start(Roll);
    c.Run(Dt60, 0.5f);
    c.cut_next = true;
    c.Run(Dt60, 0.1f);
    EXPECT_TRUE(c.ext.CutPending());
    c.Start(Roll);
    c.Update(Dt60);
    EXPECT_FALSE(c.ext.Cut());
    c.Run(Dt60, 3.0f);
    ASSERT_EQ(c.runs.size(), 2u);
    EXPECT_EQ(c.runs[0], vanilla + 4);
    EXPECT_EQ(c.runs[1], vanilla + 4);

    // At 30 fps (one update between the clip's start and its F8 at 0.05 s).
    Game h(2.0f * I::GameFrame);
    h.Run(Dt30, 0.2f);
    h.Start(LateRoll);
    h.cut_next = true;
    h.Run(Dt30, 3.0f);
    EXPECT_TRUE(h.runs.empty());
    EXPECT_EQ(h.asserts, 0);

    // Without ring data (only SpEffect 392 tells the dodge): the same through the effect.
    Game r(2.0f * I::GameFrame);
    r.ring = false;
    r.Run(Dt60, 0.2f);
    r.Start(LateRoll);
    r.cut_next = true;
    r.Run(Dt60, 3.0f);
    EXPECT_TRUE(r.runs.empty());
    r.Start(Roll);
    r.Run(Dt60, 3.0f);
    ASSERT_EQ(r.runs.size(), 1u);
    EXPECT_EQ(r.runs[0], vanilla + 4);
}

// A blow that knocks the hunter down during a cut dodge (the cut is what lets it land): the cut
// ends with the dodge, the knockdown keeps its own F8 window (the game's get-up invulnerability),
// whether its F8 follows the dodge's window at once or comes in the dodge's recovery. A cut
// request that reaches the hook only as the knockdown starts is dropped too.
TEST(IframeExtend, ACutNeverReachesAKnockdownsWindow) {
    const int knock = VanillaRun(Knockdown, Dt60);
    for (const float at : {0.1f, 0.5f}) { // inside the roll's window, in its recovery
        for (const bool late : {false, true}) {
            Game g(2.0f * I::GameFrame);
            g.Run(Dt60, 0.2f);
            g.Start(Roll);
            g.Run(Dt60, 0.05f);
            g.cut_next = !late;
            g.Run(Dt60, at);
            g.Start(Knockdown);
            g.interrupt_next = true;
            g.cut_next = late;
            g.Update(Dt60);
            EXPECT_FALSE(g.ext.Cut()) << at << " " << late;
            g.Run(Dt60, 3.0f);
            ASSERT_FALSE(g.runs.empty());
            // (No cut at all with the late request: inside the roll's window the knockdown's F8
            // continues the roll's protected run.)
            const int roll_part =
                late && at < 0.3f ? static_cast<int>(std::lround((0.05f + at) / Dt60)) : 0;
            EXPECT_EQ(g.runs.back(), knock + roll_part) << at << " " << late;
            // The next roll: whole.
            g.Start(Roll);
            g.Run(Dt60, 3.0f);
            EXPECT_EQ(g.runs.back(), VanillaRun(Roll, Dt60) + 4) << at << " " << late;
        }
    }
}

} // namespace
