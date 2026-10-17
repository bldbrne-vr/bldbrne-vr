// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "core/vr/session_pause.h"
#include "core/libraries/audio/audio_clock.h"

using namespace std::chrono_literals;

namespace {

bool AwaitWaiting(VR::SessionPauseGate& gate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!gate.Waiting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return gate.Waiting();
}

TEST(SessionPause, StartupMenuReturnAndSessionLoss) {
    VR::SessionPausePolicy policy;
    EXPECT_FALSE(policy.Update(false, true));
    EXPECT_FALSE(policy.Update(false, true));
    EXPECT_FALSE(policy.Update(true, true));
    EXPECT_TRUE(policy.Update(false, true));
    EXPECT_TRUE(policy.Update(false, true));
    EXPECT_FALSE(policy.Update(true, true));
    EXPECT_TRUE(policy.Update(false, true));
    EXPECT_FALSE(policy.Update(false, false));
    EXPECT_FALSE(policy.Update(false, true));
    EXPECT_FALSE(policy.Update(true, true));
    EXPECT_TRUE(policy.Update(false, true));
}

TEST(SessionPause, CancelBeforeFrameBoundaryRestoresClockProgress) {
    VR::SessionPauseGate gate;
    std::uint64_t now = 100;
    const auto clock = [&] { return now; };
    gate.Request(true, clock);
    now = 500;
    EXPECT_EQ(gate.ReadClock(clock), 100u);
    gate.Request(false, clock);
    gate.Wait();
    EXPECT_FALSE(gate.Paused());
    now += 20;
    EXPECT_EQ(gate.ReadClock(clock), 120u);
}

TEST(SessionPause, FrameBoundarySleepsAndExcludesPausedTime) {
    VR::SessionPauseGate gate;
    std::atomic<std::uint64_t> now{100};
    const auto clock = [&] { return now.load(); };
    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto before = gate.ReadClock(clock);
        gate.Request(true, clock);
        std::atomic_bool returned{false};
        std::jthread game([&] { gate.Wait(); returned = true; });
        EXPECT_TRUE(AwaitWaiting(gate));
        gate.Request(true, clock);
        now += 1'000;
        EXPECT_FALSE(returned.load());
        EXPECT_EQ(gate.ReadClock(clock), before);
        gate.Request(true, clock);
        EXPECT_EQ(gate.ReadClock(clock), before);
        gate.Request(false, clock);
        game.join();
        EXPECT_TRUE(returned.load());
        EXPECT_FALSE(gate.Paused());
        EXPECT_EQ(gate.ReadClock(clock), before);
        now += 10;
        EXPECT_EQ(gate.ReadClock(clock), before + 10);
    }
}

TEST(SessionPause, ResumeBeforeAnotherXrFrameExcludesTheWholePause) {
    VR::SessionPauseGate gate;
    std::uint64_t now = 100;
    const auto clock = [&] { return now; };
    gate.Request(true, clock);
    std::jthread game([&] { gate.Wait(); });
    EXPECT_TRUE(AwaitWaiting(gate));
    now = 1'100;
    EXPECT_EQ(gate.ReadClock(clock), 100u);
    gate.Request(false, clock);
    game.join();
    EXPECT_EQ(gate.ReadClock(clock), 100u);
    now += 20;
    EXPECT_EQ(gate.ReadClock(clock), 120u);
}

TEST(SessionPause, DelayedFrameBoundaryDoesNotDelayTheClockPause) {
    VR::SessionPauseGate gate;
    std::uint64_t now = 100;
    const auto clock = [&] { return now; };
    gate.Request(true, clock);
    now = 500;
    EXPECT_EQ(gate.ReadClock(clock), 100u);
    std::jthread game([&] { gate.Wait(); });
    EXPECT_TRUE(AwaitWaiting(gate));
    gate.Request(true, clock);
    now = 1'100;
    gate.Request(false, clock);
    game.join();
    EXPECT_EQ(gate.ReadClock(clock), 100u);
}

TEST(SessionPause, MultipleWaitersCountThePauseOnce) {
    VR::SessionPauseGate gate;
    std::atomic<std::uint64_t> now{100};
    const auto clock = [&] { return now.load(); };
    gate.Request(true, clock);
    std::jthread first([&] { gate.Wait(); });
    EXPECT_TRUE(AwaitWaiting(gate));
    gate.Request(true, clock);
    std::jthread second([&] { gate.Wait(); });
    now = 500;
    gate.Request(false, clock);
    first.join();
    second.join();
    EXPECT_EQ(gate.ReadClock(clock), 100u);
}

TEST(SessionPause, OnlyHostRequestsWriteTheClock) {
    VR::SessionPauseGate gate;
    const auto host = std::this_thread::get_id();
    std::uint64_t now = 100;
    unsigned clock_writes = 0;
    const auto clock = [&] {
        EXPECT_EQ(std::this_thread::get_id(), host);
        ++clock_writes;
        return now;
    };
    gate.Request(true, clock);
    EXPECT_EQ(clock_writes, 1u);
    EXPECT_TRUE(gate.Paused());
    std::jthread game([&] { gate.Wait(); });
    EXPECT_TRUE(AwaitWaiting(gate));
    EXPECT_EQ(clock_writes, 1u);
    gate.Request(true, clock);
    EXPECT_TRUE(gate.Paused());
    EXPECT_EQ(clock_writes, 1u);
    now = 1'100;
    gate.Request(false, clock);
    EXPECT_FALSE(gate.Paused());
    EXPECT_EQ(clock_writes, 2u);
    EXPECT_EQ(gate.ReadClock([&] { return now; }), 100u);
    game.join();
    now += 20;
    EXPECT_EQ(gate.ReadClock([&] { return now; }), 120u);
}

TEST(SessionPause, ProcessClockCanStartDuringHeadsetPause) {
    VR::SessionPauseGate gate;
    std::uint64_t now = 100;
    const auto clock = [&] { return now; };
    gate.Request(true, clock);
    now = 500;
    const auto initial_ptc = gate.ReadClock(clock);
    now = 1'100;
    EXPECT_EQ(gate.ReadClock(clock) - initial_ptc, 0u);
    gate.Request(false, clock);
    EXPECT_EQ(gate.ReadClock(clock) - initial_ptc, 0u);
    now += 20;
    EXPECT_EQ(gate.ReadClock(clock) - initial_ptc, 20u);
}

TEST(SessionPause, AudioClockAdvancesWhileGameClockIsFrozen) {
    VR::SessionPauseGate gate;
    const auto clock = [] { return Libraries::AudioOut::HostTimeUs(); };
    gate.Request(true, clock);
    std::jthread game([&] { gate.Wait(); });
    EXPECT_TRUE(AwaitWaiting(gate));
    gate.Request(true, clock);
    const auto game_before = gate.ReadClock(clock);
    const auto audio_before = Libraries::AudioOut::HostTimeUs();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(gate.ReadClock(clock), game_before);
    EXPECT_GT(Libraries::AudioOut::HostTimeUs(), audio_before);
    gate.Request(false, clock);
    game.join();
}

TEST(SessionPause, ClockRemainsMonotonicAcrossConcurrentResume) {
    VR::SessionPauseGate gate;
    const auto clock = [] {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    };
    std::atomic_bool stop{false};
    std::atomic_bool backwards{false};
    std::jthread reader([&] {
        auto last = gate.ReadClock(clock);
        while (!stop.load()) {
            const auto current = gate.ReadClock(clock);
            if (current < last) backwards = true;
            last = current;
        }
    });
    for (int i = 0; i < 20; ++i) {
        gate.Request(true, clock);
        std::jthread game([&] { gate.Wait(); });
        EXPECT_TRUE(AwaitWaiting(gate));
        gate.Request(true, clock);
        gate.Request(false, clock);
        game.join();
    }
    stop = true;
    reader.join();
    EXPECT_FALSE(backwards.load());
}

}
