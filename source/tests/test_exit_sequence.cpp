// SPDX-License-Identifier: GPL-2.0-or-later

// The process's end after a close (core/vr/exit_sequence.h; the user's 2026-10-09 "if I take the
// headset off I want it to keep running, but if I kill it I want it to die"): the OpenXR teardown
// runs on its own thread within a time limit (a hung runtime is left behind, the exit goes on), a
// save being written is waited for within a limit, and the watchdog ends the process if the
// rest hangs. The times here are generous for a loaded machine: only orders are asserted.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "core/vr/exit_sequence.h"

namespace {

namespace X = VR::Exit;
using namespace std::chrono_literals;

TEST(ExitSequence, BoundedStepThatFinishes) {
    std::atomic_bool ran{false};
    const auto r = X::RunBounded([&] { ran = true; }, 1000ms);
    EXPECT_TRUE(r.done);
    EXPECT_TRUE(ran.load());
    EXPECT_LT(r.ms, 500.0);
}

TEST(ExitSequence, BoundedStepThatHangsIsLeftBehind) {
    // A runtime call that never returns within the limit: the exit goes on at the limit, and the
    // step's thread may still finish later without touching anything of the caller's.
    auto release = std::make_shared<std::atomic_bool>(false);
    auto finished = std::make_shared<std::atomic_bool>(false);
    const auto start = X::Clock::now();
    const auto r = X::RunBounded(
        [release, finished] {
            while (!release->load()) {
                std::this_thread::sleep_for(1ms);
            }
            finished->store(true);
        },
        60ms);
    const double waited = X::MsSince(start);
    EXPECT_FALSE(r.done);
    EXPECT_GE(r.ms, 55.0);
    EXPECT_LT(waited, 1000.0);
    EXPECT_FALSE(finished->load());
    release->store(true); // the left-behind thread ends on its own
    for (int i = 0; i < 1000 && !finished->load(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(finished->load());
}

TEST(ExitSequence, BoundedStepThatThrowsCountsAsDone) {
    const auto r = X::RunBounded([] { throw 1; }, 1000ms);
    EXPECT_TRUE(r.done);
}

TEST(ExitSequence, NoBudgetNoStep) {
    std::atomic_bool ran{false};
    const auto r = X::RunBounded([&] { ran = true; }, 0ms);
    EXPECT_FALSE(r.done);
    EXPECT_FALSE(ran.load());
    EXPECT_FALSE(X::RunBounded({}, 100ms).done);
}

TEST(ExitSequence, WaitWhileASaveIsWritten) {
    // Not busy: no wait at all.
    const auto idle = X::WaitWhile([] { return false; }, 1000ms);
    EXPECT_TRUE(idle.done);
    EXPECT_LT(idle.ms, 50.0);
    // A save closing after ~40 ms: waited for.
    const auto start = X::Clock::now();
    const auto w = X::WaitWhile([&] { return X::MsSince(start) < 40.0; }, 2000ms, 2ms);
    EXPECT_TRUE(w.done);
    EXPECT_GE(w.ms, 39.0);
    EXPECT_LT(w.ms, 1000.0);
    // One that never closes: given up at the limit.
    const auto stuck = X::WaitWhile([] { return true; }, 50ms, 2ms);
    EXPECT_FALSE(stuck.done);
    EXPECT_GE(stuck.ms, 49.0);
    EXPECT_LT(stuck.ms, 1000.0);
}

TEST(ExitSequence, WatchdogFiresOnceUnlessDisarmed) {
    auto fired = std::make_shared<std::atomic_int>(0);
    {
        X::Watchdog dog;
        dog.Arm(40ms, [fired] { fired->fetch_add(1); });
        for (int i = 0; i < 2000 && fired->load() == 0; ++i) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(fired->load(), 1);
        std::this_thread::sleep_for(60ms);
        EXPECT_EQ(fired->load(), 1); // once
    }
    // Disarmed in time (the process ended normally): never.
    auto late = std::make_shared<std::atomic_int>(0);
    {
        X::Watchdog dog;
        dog.Arm(80ms, [late] { late->fetch_add(1); });
        std::this_thread::sleep_for(10ms);
        dog.Disarm();
    }
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(late->load(), 0);
    // Re-armed: the first is disarmed, only the second fires.
    auto first = std::make_shared<std::atomic_int>(0);
    auto second = std::make_shared<std::atomic_int>(0);
    X::Watchdog dog;
    dog.Arm(60ms, [first] { first->fetch_add(1); });
    dog.Arm(30ms, [second] { second->fetch_add(1); });
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(first->load(), 0);
    EXPECT_EQ(second->load(), 1);
}

TEST(ExitSequence, TeardownHookRunsOnceWithinItsLimit) {
    // None registered (no headset): nothing to run.
    X::ClearTeardown();
    EXPECT_FALSE(X::HasTeardown());
    const auto none = X::RunTeardown(1000ms);
    EXPECT_FALSE(none.done);
    EXPECT_EQ(none.ms, 0.0);
    // The presenter's: run once (taken), then gone.
    std::atomic_int runs{0};
    X::SetTeardown([&] { runs.fetch_add(1); });
    EXPECT_TRUE(X::HasTeardown());
    const auto r = X::RunTeardown(1000ms);
    EXPECT_TRUE(r.done);
    EXPECT_EQ(runs.load(), 1);
    EXPECT_FALSE(X::HasTeardown());
    EXPECT_FALSE(X::RunTeardown(1000ms).done);
    EXPECT_EQ(runs.load(), 1);
    // A runtime that hangs in it: the exit goes on at the limit.
    auto release = std::make_shared<std::atomic_bool>(false);
    X::SetTeardown([release] {
        while (!release->load()) {
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto start = X::Clock::now();
    const auto hung = X::RunTeardown(50ms);
    EXPECT_FALSE(hung.done);
    EXPECT_LT(X::MsSince(start), 1000.0);
    release->store(true);
    // The presenter going away waits for the teardown in progress, then clears it.
    X::ClearTeardown();
    EXPECT_FALSE(X::HasTeardown());
}

TEST(ExitSequence, QuitRequestTime) {
    X::QuitRequestedNs().store(0);
    EXPECT_EQ(X::QuitRequestedNs().load(), 0);
    const auto t = X::NowNs();
    X::QuitRequestedNs().store(t);
    EXPECT_GT(X::NowNs(), 0);
    EXPECT_GE(X::NowNs(), X::QuitRequestedNs().load());
}

} // namespace
