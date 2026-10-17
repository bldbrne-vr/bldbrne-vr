// SPDX-License-Identifier: GPL-2.0-or-later

// Controller vibration: when the presenter calls the runtime (core/vr/haptic_output.h). Drives
// VR::Haptics::Output with the presenter's pattern semantics at 120 Hz and checks the calls it
// asks for and what a runtime would then play: a steady tick is one call, a short tick lasts
// haptic_min_ms (also a new one inside the last one's hold), ramps and hums are sent in steps or
// kept alive without gaps, silence stops it, the floor raises only the ticks, gain 0 is silent,
// a failed call is tried again.

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/haptic_output.h"

namespace {

namespace H = VR::Haptics;

constexpr std::int64_t Ms = 1'000'000;
constexpr std::int64_t Period = 8'333'333; // 120 Hz
constexpr float Dt = 1.0f / 120.0f;

// The presenter's HapticPattern / HandHaptics (openxr_presenter.cpp): linear segments.
struct Segment {
    float from, to, seconds;
};
struct Pattern {
    std::vector<Segment> segments;
    float time{};
    bool playing{false};
    bool fresh{false};

    void Play(std::vector<Segment> next) {
        segments = std::move(next);
        time = 0.0f;
        playing = true;
        fresh = true;
    }
    float Sample(float dt) {
        if (!playing) {
            return 0.0f;
        }
        float t = time;
        time += dt;
        for (const auto& s : segments) {
            if (t < s.seconds) {
                return s.from + (s.to - s.from) * (t / s.seconds);
            }
            t -= s.seconds;
        }
        playing = false;
        return 0.0f;
    }
    float SteadyFor() const {
        if (!playing) {
            return 0.0f;
        }
        float t = time;
        for (size_t i = 0; i < segments.size(); ++i) {
            const auto& s = segments[i];
            if (t < s.seconds) {
                if (s.from != s.to) {
                    return 0.0f;
                }
                float left = s.seconds - t;
                for (size_t j = i + 1; j < segments.size(); ++j) {
                    if (segments[j].from != s.from || segments[j].to != s.from) {
                        break;
                    }
                    left += segments[j].seconds;
                }
                return left;
            }
            t -= s.seconds;
        }
        return 0.0f;
    }
};

struct CallRecord {
    std::int64_t t;
    H::Call call;
    float amplitude;
    std::int64_t duration;
};

// One hand at 120 Hz, with a runtime that plays each call until its duration or a stop.
struct Sim {
    H::Output out{};
    H::Config config{};
    Pattern pattern{};
    float hum{};
    float rumble{};
    bool own{false}; // the pattern owns the hand (Input::own) while it plays
    std::int64_t now{1'000 * Ms};
    std::int64_t start{1'000 * Ms};
    int fail_next{0}; // this many applies fail
    std::vector<CallRecord> calls;
    bool motor{false};
    std::int64_t motor_until{};
    float motor_amp{};
    std::vector<float> played; // the runtime's amplitude at each frame (0 = still)
    std::vector<float> asked;  // the level Step decided for each frame
    int gaps{0};

    Sim() {
        config.period_ns = Period;
    }
    std::int64_t T() const {
        return now - start;
    }
    void Frame(std::int64_t step = Period) {
        H::Input in{};
        in.fresh = std::exchange(pattern.fresh, false);
        in.steady_s = pattern.SteadyFor();
        in.tick = pattern.Sample(Dt);
        in.hum = hum;
        in.rumble = rumble;
        in.own = own && pattern.playing;
        const H::Decision d = out.Step(in, config, now);
        // A call that continues a vibration (the last frame asked for one too) after the last
        // call already ran out: the motor paused in between.
        if (d.call == H::Call::Apply && motor && !asked.empty() && asked.back() > 0.0f &&
            d.level > 0.0f && motor_until < now) {
            ++gaps;
        }
        asked.push_back(d.level);
        if (d.call == H::Call::Apply) {
            calls.push_back({T(), d.call, d.amplitude, d.duration_ns});
            const bool ok = fail_next <= 0;
            fail_next = std::max(fail_next - 1, 0);
            if (ok) {
                motor = true;
                motor_until = now + d.duration_ns;
                motor_amp = d.amplitude;
            }
            out.Applied(ok, d.amplitude, d.duration_ns, now);
        } else if (d.call == H::Call::Stop) {
            calls.push_back({T(), d.call, 0.0f, 0});
            motor = false;
        }
        played.push_back(motor && now < motor_until ? motor_amp : 0.0f);
        now += step;
    }
    void Run(int frames) {
        for (int i = 0; i < frames; ++i) {
            Frame();
        }
    }
    int Count(H::Call call) const {
        int n = 0;
        for (const auto& c : calls) {
            n += c.call == call ? 1 : 0;
        }
        return n;
    }
};

TEST(HapticOutput, SteadyTickIsOneCallThenAStop) {
    Sim s;
    s.pattern.Play({{0.6f, 0.6f, 0.040f}});
    s.Run(12);
    ASSERT_EQ(s.Count(H::Call::Apply), 1);
    EXPECT_EQ(s.calls[0].t, 0);
    EXPECT_NEAR(s.calls[0].amplitude, 0.3f + 0.7f * 0.6f, 1e-5f);
    EXPECT_NEAR(static_cast<double>(s.calls[0].duration), 40.0 * Ms + 2 * Period, 0.2 * Ms);
    ASSERT_EQ(s.Count(H::Call::Stop), 1);
    EXPECT_GE(s.calls[1].t, 40 * Ms);
    EXPECT_LE(s.calls[1].t, 40 * Ms + Period);
    for (int f = 0; f * Period < 40 * Ms; ++f) {
        EXPECT_GT(s.played[f], 0.7f); // on through the whole tick
    }
    EXPECT_FALSE(s.out.vibrating);
}

TEST(HapticOutput, ShortTickLastsMinOn) {
    Sim s;
    s.pattern.Play({{0.15f, 0.15f, 0.008f}});
    s.Run(8);
    ASSERT_GE(s.Count(H::Call::Apply), 1);
    EXPECT_NEAR(s.calls[0].amplitude, 0.3f + 0.7f * 0.15f, 1e-5f);
    EXPECT_GE(s.calls[0].duration, 25 * Ms);
    for (const auto& c : s.calls) {
        if (c.call == H::Call::Stop) {
            EXPECT_GE(c.t, 25 * Ms); // never cut before haptic_min_ms
        }
    }
    for (int f = 0; f * Period < 25 * Ms; ++f) {
        EXPECT_GT(s.played[f], 0.0f);
    }
    EXPECT_FALSE(s.out.vibrating);
}

TEST(HapticOutput, NewTickInsideTheHoldGetsItsOwnMinOn) {
    // The transform's detents: an 8 ms tick, the next one 25 ms later (the first still held).
    Sim s;
    s.pattern.Play({{0.3f, 0.3f, 0.008f}});
    s.Run(3);
    s.pattern.Play({{0.3f, 0.3f, 0.008f}});
    const std::int64_t second = s.T();
    s.Run(8);
    ASSERT_EQ(s.Count(H::Call::Apply), 2);
    EXPECT_EQ(s.calls[1].t, second);
    EXPECT_GE(s.calls[1].duration, 25 * Ms);
    for (const auto& c : s.calls) {
        if (c.call == H::Call::Stop) {
            EXPECT_GE(c.t, second + 25 * Ms);
        }
    }
    for (int f = 3; (f - 3) * Period < 25 * Ms; ++f) {
        EXPECT_GT(s.played[f], 0.0f);
    }
}

TEST(HapticOutput, RampIsSentInStepsAndStopped) {
    // The pistol's thump: 1.0 for 25 ms, then 0.55 -> 0 over 90 ms.
    Sim s;
    s.pattern.Play({{1.0f, 1.0f, 0.025f}, {0.55f, 0.0f, 0.090f}});
    s.Run(18);
    const int applies = s.Count(H::Call::Apply);
    EXPECT_GE(applies, 3);
    EXPECT_LE(applies, 8); // vs 14 frames with sound
    EXPECT_EQ(s.Count(H::Call::Stop), 1);
    EXPECT_EQ(s.calls.back().call, H::Call::Stop);
    EXPECT_EQ(s.gaps, 0);
    for (size_t f = 0; f < s.asked.size(); ++f) {
        if (s.asked[f] > 0.0f) {
            EXPECT_GT(s.played[f], 0.0f); // never a gap while it should vibrate
            EXPECT_NEAR(s.played[f], s.asked[f], 0.16f);
        }
    }
}

TEST(HapticOutput, LateFrameAtTheEndOfASteadyTickLeavesNoGap) {
    // A 30 ms thump that fades, the first frame after its steady part 80% late (a hitch).
    Sim s;
    s.pattern.Play({{1.0f, 1.0f, 0.030f}, {0.55f, 0.0f, 0.090f}});
    s.Run(3);
    s.Frame(Period * 18 / 10);
    s.Run(14);
    EXPECT_EQ(s.gaps, 0);
    for (size_t f = 0; f < s.asked.size(); ++f) {
        if (s.asked[f] > 0.0f) {
            EXPECT_GT(s.played[f], 0.0f);
        }
    }
}

TEST(HapticOutput, HumIsKeptAliveWithoutGapsAndNotRaised) {
    Sim s;
    s.hum = 0.06f;
    s.Run(120); // 1 s
    const int applies = s.Count(H::Call::Apply);
    EXPECT_GE(applies, 9);
    EXPECT_LE(applies, 13);
    for (const auto& c : s.calls) {
        EXPECT_NEAR(c.amplitude, 0.06f, 1e-6f);
        EXPECT_EQ(c.duration, s.config.keepalive_ns);
    }
    EXPECT_EQ(s.gaps, 0);
    for (float p : s.played) {
        EXPECT_NEAR(p, 0.06f, 1e-6f);
    }
    s.hum = 0.0f;
    s.Run(4);
    EXPECT_EQ(s.calls.back().call, H::Call::Stop);
    EXPECT_EQ(s.played.back(), 0.0f);
}

TEST(HapticOutput, RisingHumStepsByFivePercentOrFifteenPercent) {
    Sim s;
    for (int f = 0; f < 60; ++f) { // 0 -> 0.5 over 0.5 s
        s.hum = 0.5f * static_cast<float>(f) / 60.0f;
        s.Frame();
    }
    const int applies = s.Count(H::Call::Apply);
    EXPECT_GE(applies, 7);
    EXPECT_LE(applies, 14);
    for (size_t f = 0; f < s.asked.size(); ++f) {
        if (s.asked[f] > 0.0f) {
            EXPECT_NEAR(s.played[f], s.asked[f], 0.08f);
        }
    }
}

TEST(HapticOutput, GameRumbleIsNotRaisedAndTheStrongestWins) {
    Sim s;
    s.rumble = 0.5f;
    s.pattern.Play({{0.1f, 0.1f, 0.030f}});
    s.Frame();
    ASSERT_EQ(s.Count(H::Call::Apply), 1);
    EXPECT_NEAR(s.calls[0].amplitude, 0.5f, 1e-6f); // rumble 0.5 > tick 0.1 -> 0.37
    Sim t;
    t.rumble = 0.2f;
    t.pattern.Play({{0.1f, 0.1f, 0.030f}});
    t.Frame();
    EXPECT_NEAR(t.calls[0].amplitude, 0.37f, 1e-5f);
}

TEST(HapticOutput, GainZeroIsSilent) {
    Sim s;
    s.config.gain = 0.0f;
    s.pattern.Play({{1.0f, 1.0f, 0.1f}});
    s.hum = 0.5f;
    s.rumble = 0.8f;
    s.Run(20);
    EXPECT_TRUE(s.calls.empty());
}

TEST(HapticOutput, GainScalesBeforeTheFloor) {
    Sim s;
    s.config.gain = 1.5f;
    s.pattern.Play({{0.5f, 0.5f, 0.05f}});
    s.Frame();
    EXPECT_NEAR(s.calls[0].amplitude, 0.3f + 0.7f * 0.75f, 1e-5f);
    Sim t;
    t.config.gain = 1.5f;
    t.pattern.Play({{0.9f, 0.9f, 0.05f}});
    t.Frame();
    EXPECT_NEAR(t.calls[0].amplitude, 1.0f, 1e-6f); // clamped
}

TEST(HapticOutput, FailedCallIsTriedAgain) {
    Sim s;
    s.fail_next = 3;
    s.pattern.Play({{0.8f, 0.8f, 0.100f}});
    s.Run(6);
    ASSERT_GE(s.Count(H::Call::Apply), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(s.calls[static_cast<size_t>(i)].t, i * Period); // every frame until one works
    }
    EXPECT_GT(s.played[3], 0.0f);
    EXPECT_TRUE(s.out.vibrating);
}

TEST(HapticOutput, RawModeWithoutFloorAndMinimum) {
    Sim s;
    s.config.floor = 0.0f;
    s.config.min_on_ns = 0;
    s.pattern.Play({{0.15f, 0.15f, 0.008f}});
    s.Run(4);
    ASSERT_EQ(s.Count(H::Call::Apply), 1);
    EXPECT_NEAR(s.calls[0].amplitude, 0.15f, 1e-6f);
    EXPECT_NEAR(static_cast<double>(s.calls[0].duration), 8.0 * Ms + 2 * Period, 0.2 * Ms);
}

TEST(HapticOutput, DoubleTickStaysTwoTicks) {
    // Repeat(2, 0.15, 10 ms, 50 ms gap): the deny pattern.
    Sim s;
    s.pattern.Play({{0.15f, 0.15f, 0.010f}, {0.0f, 0.0f, 0.050f}, {0.15f, 0.15f, 0.010f}});
    s.Run(14);
    EXPECT_EQ(s.Count(H::Call::Apply), 2);
    int bursts = 0;
    bool on = false;
    for (float p : s.played) {
        bursts += (p > 0.0f && !on) ? 1 : 0;
        on = p > 0.0f;
    }
    EXPECT_EQ(bursts, 2);
}

TEST(HapticOutput, LongSteadyPulseIsOneCall) {
    // The stereo switch's "mono" buzz: 0.3 for 250 ms.
    Sim s;
    s.pattern.Play({{0.3f, 0.3f, 0.250f}});
    s.Run(34);
    EXPECT_EQ(s.Count(H::Call::Apply), 1);
    EXPECT_EQ(s.Count(H::Call::Stop), 1);
}

TEST(HapticOutput, ExpiredVibrationNeedsNoStop) {
    // A tick that ran out by its own duration: going silent later calls nothing.
    Sim s;
    s.config.min_on_ns = 25 * Ms;
    s.pattern.Play({{0.5f, 0.5f, 0.004f}});
    s.Run(10);
    EXPECT_EQ(s.Count(H::Call::Apply), 1);
    EXPECT_EQ(s.Count(H::Call::Stop), 0);
    EXPECT_FALSE(s.out.vibrating);
}

// An impact that owns its hand: the game's rumble (0.8, an enemy's blows) and a swing's arc hum
// are left out while it plays, so its crack and decay keep their shape; both come back after it.
TEST(HapticOutput, OwnedImpactMutesRumbleAndHumWhileItPlays) {
    Sim s;
    s.rumble = 0.8f;
    s.hum = 0.3f;
    s.Run(6); // the rumble alone
    EXPECT_NEAR(s.asked.back(), 0.8f, 1e-5f);
    const H::Combat::Shape sh = H::Combat::MeleeImpact(0.4f, 1.0f);
    s.own = true;
    s.pattern.Play({{sh.peak, sh.peak, sh.hold_ms * 1e-3f}, {sh.tail_from, 0.0f, sh.tail_ms * 1e-3f}});
    const size_t at = s.asked.size();
    s.Run(4);
    EXPECT_NEAR(s.asked[at], H::Combat::Played(sh.peak), 1e-4f); // the crack
    // The tail ramps down from the tail's start, under the rumble's 0.8 near its end: still the
    // tail, never the rumble.
    s.Run(8);
    EXPECT_LT(s.asked[at + 11], 0.8f);
    EXPECT_GT(s.asked[at + 11], 0.0f);
    s.Run(10); // played out: the rumble back
    EXPECT_NEAR(s.asked.back(), 0.8f, 1e-5f);
}

TEST(HapticOutput, UnownedTickStaysUnderTheRumble) {
    // A swing's tick (0.2, 0.44 played) under the rumble: the rumble's 0.8 wins, as before.
    Sim s;
    s.rumble = 0.8f;
    s.Run(3);
    s.pattern.Play({{0.2f, 0.2f, 0.010f}});
    s.Run(2);
    EXPECT_NEAR(s.asked.back(), 0.8f, 1e-5f);
}

TEST(HapticCombat, HierarchyKeepsEachEventApart) {
    namespace C = H::Combat;
    const float swing_tick = C::Played(0.2f);
    const float light = C::Played(C::MeleeImpact(C::ImpactWeight(94.6f, 0.09f, false), 1.0f).peak);
    const float heavy = C::Played(C::MeleeImpact(C::ImpactWeight(194.5f, 0.12f, false), 1.0f).peak);
    const float knock = C::Played(C::MeleeKnock(1.0f).peak);
    const float taken_scratch = C::Played(C::HitTaken(0.02f, 1.0f, 1.0f).peak);
    const float taken_big = C::Played(C::HitTaken(0.30f, 1.0f, 1.0f).peak);
    const float taken_far = C::Played(C::HitTaken(0.02f, 0.3f, 1.0f).peak);
    const float gun_hit = C::Played(C::GunHit(1.0f).peak);
    const float throw_hit = C::Played(C::ThrowHit(1.0f).peak);
    // The impact: near full for any hit, at least 0.5 above the swing's tick.
    EXPECT_GE(light, 0.93f);
    EXPECT_GE(heavy, light);
    EXPECT_GE(light - swing_tick, 0.45f);
    // A blow on the hunter: small (0.43 .. 0.6), well under the weapon hand's impact.
    EXPECT_GE(taken_scratch, 0.43f);
    EXPECT_LE(taken_big, 0.6f);
    EXPECT_GT(taken_big, taken_scratch);
    EXPECT_LT(taken_far, taken_scratch);
    EXPECT_GE(taken_far, 0.42f);
    EXPECT_LE(light - taken_big, 0.5f + 1e-3f);
    EXPECT_GE(light - taken_big, 0.35f);
    // A blocked blow, a bullet or a thrown item connecting: ticks, under the impact.
    EXPECT_LT(knock, 0.6f);
    EXPECT_LT(gun_hit, 0.6f);
    EXPECT_LT(throw_hit, gun_hit);
    EXPECT_GT(throw_hit, swing_tick);
}

TEST(HapticCombat, ImpactScalesWithTheHitsWeight) {
    namespace C = H::Combat;
    // The user's 2026-10-06 hits: a Saw Cleaver light (95 damage, hit stop 0.09), its heavy (123),
    // a Hunter Axe heavy (194, 0.12), two-handed.
    const float nick = C::ImpactWeight(94.6f, 0.09f, false);
    const float cleaver_heavy = C::ImpactWeight(122.7f, 0.09f, false);
    const float axe = C::ImpactWeight(194.5f, 0.12f, false);
    EXPECT_LT(nick, cleaver_heavy);
    EXPECT_LT(cleaver_heavy, axe);
    EXPECT_GT(C::ImpactWeight(94.6f, 0.09f, true), nick);
    EXPECT_LE(C::ImpactWeight(1e9f, 1.0f, true), 1.0f);
    EXPECT_GE(C::ImpactWeight(-5.0f, 0.0f, false), 0.0f);
    EXPECT_GE(C::ImpactWeight(std::nanf(""), std::nanf(""), false), 0.0f);
    const C::Shape a = C::MeleeImpact(nick, 1.0f);
    const C::Shape b = C::MeleeImpact(axe, 1.0f);
    const float len_a = a.hold_ms + a.tail_ms;
    const float len_b = b.hold_ms + b.tail_ms;
    EXPECT_GE(len_a, 80.0f); // ~90 .. 130 ms: an impact, not a buzz
    EXPECT_LE(len_b, 135.0f);
    EXPECT_GT(len_b, len_a + 20.0f);
    EXPECT_GT(b.tail_from, a.tail_from);
    EXPECT_LT(a.tail_from, a.peak); // a sharp attack, then the decay
    EXPECT_NEAR(C::MeleeImpact(axe, 0.0f).peak, 0.0f, 1e-6f); // haptic_melee_hit=0: none
    EXPECT_LE(C::MeleeImpact(axe, 1.5f).peak, 1.0f);
}

TEST(HapticCombat, HitTakenIsShortOnBothHands) {
    namespace C = H::Combat;
    const C::Shape s = C::HitTaken(0.1f, 1.0f, 1.0f);
    EXPECT_GE(s.hold_ms + s.tail_ms, 60.0f);
    EXPECT_LE(s.hold_ms + s.tail_ms, 80.0f);
    EXPECT_NEAR(C::HitTaken(0.1f, 1.0f, 0.0f).peak, 0.0f, 1e-6f);
    EXPECT_LE(C::HitTaken(1.0f, 1.0f, 1.5f).peak, 0.6f);
}

// The user's 22:40 session (2026-10-06): the impact's 24 ms crack at 0.96 was no longer than the
// swing's own tick (0.44, 25 ms) and did not read as a hit. Now a landed hit is full (1.0 played)
// for 65 .. 80 ms, then decays: 145 .. 180 ms in all for the lights, a heavy one adds the crunch.
TEST(HapticCombat, LandedHitIsFullForSixtyFiveMsThenDecays) {
    namespace C = H::Combat;
    const float swing_tick = C::Played(0.2f);
    const float nick = C::ImpactWeight(37.0f, 0.09f, false);   // the user's lightest (#19)
    const float light = C::ImpactWeight(83.0f, 0.09f, false);  // a Saw Cleaver light (#4)
    const float heavy = C::ImpactWeight(134.0f, 0.09f, false); // its heavy (#18)
    const float axe = C::ImpactWeight(194.5f, 0.12f, false);
    for (const float w : {nick, light}) {
        const C::Shape s = C::MeleeImpact(w, 1.0f, 65.0f, true);
        EXPECT_NEAR(C::Played(s.peak), 1.0f, 1e-5f);
        EXPECT_GE(s.hold_ms, 65.0f);
        EXPECT_LE(s.hold_ms, 80.0f);
        EXPECT_EQ(s.crunch, 0.0f) << w; // a light hit: one blow
        EXPECT_GE(C::ShapeMs(s), 145.0f);
        EXPECT_LE(C::ShapeMs(s), 180.0f);
        EXPECT_GE(C::Played(s.tail_from) - swing_tick, 0.35f); // the decay starts well above it
        EXPECT_LT(s.tail_from, s.peak);
    }
    EXPECT_GT(C::MeleeImpact(light, 1.0f, 65.0f, true).hold_ms,
              C::MeleeImpact(nick, 1.0f, 65.0f, true).hold_ms);
    for (const float w : {heavy, axe, C::ImpactWeight(83.0f, 0.09f, true)}) {
        const C::Shape s = C::MeleeImpact(w, 1.0f, 65.0f, true);
        EXPECT_GE(w, C::ImpactCrunchWeight);
        EXPECT_NEAR(C::Played(s.peak), 1.0f, 1e-5f);
        EXPECT_GE(s.hold_ms, 65.0f);
        EXPECT_GE(s.crunch_gap_ms, 35.0f); // a real gap: two blows, not one long buzz
        EXPECT_GE(s.crunch_ms, 28.0f);     // and a crunch long enough not to blur away
        EXPECT_GE(C::Played(s.crunch), 0.85f);
        EXPECT_LE(C::ShapeMs(s), 230.0f);
        // haptic_melee_crunch=0: one blow.
        EXPECT_EQ(C::MeleeImpact(w, 1.0f, 65.0f, false).crunch, 0.0f);
    }
    // The strength (haptic_melee_hit): 0 none, below 1 lower, above 1 a longer full part.
    EXPECT_EQ(C::MeleeImpact(light, 0.0f, 65.0f, true).peak, 0.0f);
    EXPECT_NEAR(C::MeleeImpact(light, 0.5f, 65.0f, true).peak, 0.5f, 1e-6f);
    EXPECT_GT(C::MeleeImpact(light, 1.5f, 65.0f, true).hold_ms,
              C::MeleeImpact(light, 1.0f, 65.0f, true).hold_ms + 30.0f);
    EXPECT_LE(C::MeleeImpact(light, 1.5f, 65.0f, true).tail_from, 1.0f);
    // haptic_melee_hit_ms=0: the old 20 .. 30 ms crack, never a crunch.
    const C::Shape old = C::MeleeImpact(heavy, 1.0f, 0.0f, true);
    const C::Shape before = C::MeleeImpact(heavy, 1.0f);
    EXPECT_EQ(old.hold_ms, before.hold_ms);
    EXPECT_EQ(old.peak, before.peak);
    EXPECT_EQ(old.crunch, 0.0f);
    EXPECT_LE(old.hold_ms, 30.0f);
}

TEST(HapticCombat, ImpactSegmentsArePaddedToTheGuard) {
    namespace C = H::Combat;
    C::Segment seg[8]{};
    const C::Shape light = C::MeleeImpact(0.35f, 1.0f, 65.0f, true);
    int n = C::ShapeSegments(light, C::ImpactGuardMs, seg);
    ASSERT_EQ(n, 3); // full, the decay, the silent rest of the guard
    float total = 0.0f;
    for (int i = 0; i < n; ++i) {
        total += seg[i].ms;
    }
    EXPECT_NEAR(total, C::ImpactGuardMs, 1e-3f);
    EXPECT_EQ(seg[2].from, 0.0f);
    EXPECT_EQ(seg[2].to, 0.0f);
    const C::Shape heavy = C::MeleeImpact(0.8f, 1.0f, 65.0f, true);
    n = C::ShapeSegments(heavy, C::ImpactGuardMs, seg);
    ASSERT_EQ(n, 5); // full, the drop, the gap, the crunch, its fade (longer than the guard)
    EXPECT_EQ(seg[2].from, 0.0f);
    EXPECT_GT(seg[3].from, 0.8f);
    // No guard (a hit taken, a bullet's tick): the shape alone, as before.
    EXPECT_EQ(C::ShapeSegments(C::HitTaken(0.1f, 1.0f, 1.0f), 0.0f, seg), 2);
    EXPECT_EQ(C::ShapeSegments(C::GunHit(1.0f), 0.0f, seg), 1);
}

TEST(HapticCombat, OnlyAnotherImpactReplacesAPlayingImpact) {
    namespace C = H::Combat;
    EXPECT_FALSE(C::Replaces(2, true, 2, false)); // the same swing's wall clank
    EXPECT_FALSE(C::Replaces(2, true, 1, false)); // the next swing's tick
    EXPECT_FALSE(C::Replaces(2, true, 3, false)); // a refused swing's buzz
    EXPECT_TRUE(C::Replaces(2, true, 2, true));   // a later swing's hit
    EXPECT_TRUE(C::Replaces(2, true, 3, true));   // a visceral
    EXPECT_TRUE(C::Replaces(2, true, 4, false));  // the startup test
    // Everything else: the newer wins unless the playing one has a higher priority (as before).
    EXPECT_TRUE(C::Replaces(2, false, 2, false));
    EXPECT_FALSE(C::Replaces(2, false, 1, false));
    EXPECT_TRUE(C::Replaces(1, false, 2, true));
    EXPECT_TRUE(C::Replaces(3, false, 2, true));  // a refused swing's double tick: the hit wins
    EXPECT_FALSE(C::Replaces(3, true, 2, true)); // a visceral playing: a swing's hit waits
}

// The user's hit #4 (22:40 session) replayed at 120 Hz on the weapon hand: the swing's tick at
// its commit (34 ms before the hit), THE impact, the same swing's wall clank 17 ms after it (the
// log's order), the arc's hum and the game's rumble (0.8) under it all, the next swing's tick at
// 117 ms, a refused swing's buzz at 150 ms. The controller plays 1.0 from the impact for at least
// 60 ms in one call, nothing else asks for the hand until the guard's 200 ms are over, and a wall
// clank after it plays again. Before (no guard), the clank replaced the crack at 17 ms. And the
// arena mock's #18: a refused swing's double tick (priority 3) still playing when the hit lands
// no longer swallows the impact.
TEST(HapticCombat, UsersHitThenWallClankKeepsTheImpact) {
    namespace C = H::Combat;
    struct Slot {
        int priority{0};
        bool impact{false};
    };
    const auto segs = [](const C::Shape& sh, float guard) {
        C::Segment seg[8]{};
        const int n = C::ShapeSegments(sh, guard, seg);
        std::vector<Segment> out;
        for (int i = 0; i < n; ++i) {
            out.push_back({seg[i].from, seg[i].to, seg[i].ms * 1e-3f});
        }
        return out;
    };
    const std::vector<Segment> tick{{0.2f, 0.2f, 0.010f}};
    const std::vector<Segment> clank{{0.9f, 0.9f, 0.025f}, {0.0f, 0.0f, 0.015f},
                                     {0.35f, 0.35f, 0.010f}, {0.0f, 0.0f, 0.015f},
                                     {0.2f, 0.2f, 0.010f}};
    const std::vector<Segment> buzz{{0.10f, 0.10f, 0.045f}, {0.10f, 0.03f, 0.075f}};
    const std::vector<Segment> deny{{0.15f, 0.15f, 0.010f}, {0.0f, 0.0f, 0.050f},
                                    {0.15f, 0.15f, 0.010f}};
    for (const bool guarded : {true, false}) {
        Sim s;
        Slot slot{};
        int refused = 0;
        bool owned = false; // the playing pattern owns the hand (HapticPattern::own)
        const auto offer = [&](const std::vector<Segment>& next, int priority, bool impact,
                               bool own) {
            if (s.pattern.playing && !C::Replaces(slot.priority, slot.impact, priority, impact)) {
                ++refused;
                return;
            }
            s.pattern.Play(next);
            slot = {priority, impact};
            owned = own;
        };
        const float weight = C::ImpactWeight(83.36f, 0.09f, false);
        const C::Shape impact = guarded ? C::MeleeImpact(weight, 1.0f, 65.0f, true)
                                        : C::MeleeImpact(weight, 1.0f);
        s.hum = 0.3f; // the big swing's arc
        s.rumble = 0.8f;
        // Frames at 120 Hz; the hit at frame 4 (34 ms after the swing's tick at frame 0).
        const int hit = 4;
        size_t hit_index = 0;
        size_t hit_call = 0;
        for (int f = 0; f < 40; ++f) {
            if (f == 0) {
                offer(tick, 1, false, false);
            } else if (f == 1 && guarded) {
                offer(deny, 3, false, false); // the swing before, refused: its double tick
            } else if (f == hit) {
                hit_index = s.asked.size();
                hit_call = s.calls.size();
                // The old impact: owned, not guarded.
                offer(segs(impact, guarded ? C::ImpactGuardMs : 0.0f), 2, guarded, true);
            } else if (f == hit + 2) {
                offer(clank, 2, false, false); // the WALL 17 ms after the HIT
            } else if (f == hit + 14) {
                offer(tick, 1, false, false); // the next swing's commit
            } else if (f == hit + 18) {
                offer(buzz, 3, false, false); // a refused swing
            } else if (f == hit + 26) {
                offer(clank, 2, false, false); // 217 ms: the guard is over
            }
            s.own = owned;
            s.Frame();
        }
        if (!guarded) {
            // Before: the clank cut the 24 ms crack at 17 ms (its 0.9, 0.93 played, took over).
            EXPECT_NEAR(s.asked[hit_index + 2], C::Played(0.9f), 1e-4f);
            continue;
        }
        EXPECT_EQ(refused, 3); // the clank, the tick, the buzz inside the guard (not the hit)
        // The impact's call: 1.0 for its whole full part (>= 60 ms) in one call.
        ASSERT_GT(s.calls.size(), hit_call);
        const CallRecord& first = s.calls[hit_call];
        EXPECT_EQ(first.call, H::Call::Apply);
        EXPECT_NEAR(first.amplitude, 1.0f, 1e-5f);
        EXPECT_GE(first.duration, 60 * Ms);
        // Played 1.0 on every frame of the first 60 ms, no other call meanwhile.
        for (size_t i = hit_index; i < hit_index + 8; ++i) {
            EXPECT_NEAR(s.played[i], 1.0f, 1e-5f) << "frame " << i - hit_index;
        }
        ASSERT_GT(s.calls.size(), hit_call + 1);
        EXPECT_GE(s.calls[hit_call + 1].t - first.t, 60 * Ms);
        // Until 200 ms: only the impact's own decay (never the clank's 0.93, the rumble's 0.8 or
        // the hum's 0.3 coming back early: below the tail's level, then silence).
        float prev = 1.0f;
        for (size_t i = hit_index; i < hit_index + 24; ++i) {
            EXPECT_LE(s.asked[i], prev + 1e-5f) << "frame " << i - hit_index;
            prev = s.asked[i];
        }
        EXPECT_EQ(s.asked[hit_index + 23], 0.0f); // the guard's silent rest
        // After the guard the clank plays again.
        EXPECT_NEAR(s.asked[hit_index + 26], C::Played(0.9f), 1e-4f);
    }
}

} // namespace
