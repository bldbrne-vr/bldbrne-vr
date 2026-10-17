// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_LAZY_WAKE (scratchpad/fast-renderer/byte_ring.h, the stage A -> B draw pipe): a thread
// sleeping in Drain/Wait/Space is woken once, as the consumer takes the packet that reaches its
// target, instead of after every packet. With threads: a sleeper is woken once and spins through
// that packet; producers, a consumer that stalls now and then, and sleepers with mixed targets
// finish with no lost wakeup in every mode (a hang fails the test); the verify mode finds no lost
// wake, and finds them all when the consumer drops its targeted wakes. FAST_CPU_IDLE_CLOCK's
// windows (busy_clock.h) add up to the busy time and count every packet once.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "scratchpad/fast-renderer/busy_clock.h"
#include "scratchpad/fast-renderer/byte_ring.h"

namespace {

using FastRender::ByteRing;
using FastRender::RingWake;

// A lost wakeup hangs a thread for good: report it and stop the process (it cannot be joined).
template <class Body>
void FinishesWithin(std::chrono::seconds limit, Body&& body) {
    std::packaged_task<void()> task(std::forward<Body>(body));
    auto done = task.get_future();
    std::thread runner(std::move(task));
    if (done.wait_for(limit) != std::future_status::ready) {
        std::fprintf(stderr, "FAILED: still waiting after %lld s: a lost wakeup\n",
                     static_cast<long long>(limit.count()));
        std::fflush(stderr);
        std::_Exit(1);
    }
    runner.join();
}

// Holds the calling thread for `us` microseconds without sleeping, so a sleeper's spin runs out.
void Stall(int us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {
        _mm_pause();
    }
}

void WaitForSleeps(const ByteRing<4096>& ring, std::uint64_t sleeps) {
    while (ring.Counts().sleeps < sleeps) {
        std::this_thread::yield();
    }
}

// 64 packets of one 64-byte slot each (48 payload bytes + the header), published at once.
template <std::size_t Capacity>
std::vector<std::uint64_t> PublishSlots(ByteRing<Capacity>& ring, int count) {
    std::vector<std::uint64_t> ends;
    for (int i = 0; i < count; ++i) {
        std::memset(ring.Begin(48), i, 48);
        ring.Commit();
        ends.push_back(ring.Published());
    }
    return ends;
}

// Sleepers wait for the given packets of 64; the consumer starts once they all sleep.
struct Sleepers {
    bool lost = false, reached = true;
    ByteRing<4096>::WakeCounts counts{};
};
Sleepers RunSleepers(RingWake mode, bool drop_target_wakes, std::vector<int> packets) {
    ByteRing<4096> ring{mode};
    if (drop_target_wakes) {
        ring.DropTargetWakesForTest();
    }
    const auto ends = PublishSlots(ring, 64);
    Sleepers result;
    std::atomic<bool> lost{false}, reached{true};
    FinishesWithin(std::chrono::seconds(30), [&] {
        std::vector<std::thread> sleepers;
        for (const int packet : packets) {
            sleepers.emplace_back([&, packet] {
                const std::uint64_t target = ends[packet - 1];
                if (ring.Wait(target)) {
                    lost = true;
                }
                if (!ring.Reached(target)) {
                    reached = false;
                }
            });
        }
        WaitForSleeps(ring, packets.size());
        std::thread consumer([&] {
            for (int i = 0; i < 64; ++i) {
                while (ring.Peek().empty()) {
                    std::this_thread::yield();
                }
                Stall(20); // a packet's handler, shorter than a woken sleeper's spin
                ring.Release();
            }
        });
        consumer.join();
        for (auto& sleeper : sleepers) {
            sleeper.join();
        }
    });
    result.lost = lost;
    result.reached = reached;
    result.counts = ring.Counts();
    return result;
}

TEST(RingWake, TargetWakesASleeperOnceAtItsTarget) {
    const auto run = RunSleepers(RingWake::Target, false, {40});
    EXPECT_TRUE(run.reached);
    EXPECT_FALSE(run.lost);
    EXPECT_EQ(run.counts.sleeps, 1u);
    // Woken as packet 40 is taken, it spins through it. A second wake (at the target) only if the
    // packet outlasts the spin, as on a loaded machine. Every packet until 40 woke it before.
    EXPECT_LE(1u, run.counts.wakes);
    EXPECT_LE(run.counts.wakes, 2u);
    EXPECT_LE(40u, run.counts.would_wake);
}

TEST(RingWake, TargetWakesTwoSleepersAtTheirTargets) {
    // The wake at 20 wakes both; the one waiting for 40 spins, registers again and sleeps on.
    const auto run = RunSleepers(RingWake::Target, false, {40, 20});
    EXPECT_TRUE(run.reached);
    EXPECT_FALSE(run.lost);
    EXPECT_EQ(run.counts.sleeps, 2u);
    EXPECT_LE(2u, run.counts.wakes);
    EXPECT_LE(run.counts.wakes, 4u);
    EXPECT_LE(40u, run.counts.would_wake);
}

TEST(RingWake, VerifyFindsNoLostWakeForTwoSleepers) {
    const auto run = RunSleepers(RingWake::Verify, false, {40, 20});
    EXPECT_TRUE(run.reached);
    EXPECT_FALSE(run.lost);
    EXPECT_EQ(run.counts.verified, 2u);
    EXPECT_EQ(run.counts.mismatches, 0u);
    EXPECT_LE(40u, run.counts.wakes); // the every-packet wakes are still made
}

TEST(RingWake, VerifyFindsDroppedTargetWakes) {
    // The consumer records no targeted wake: the sleeper still returns (the every-packet wakes)
    // and reports the wake it was owed.
    const auto run = RunSleepers(RingWake::Verify, true, {40, 20});
    EXPECT_TRUE(run.reached);
    EXPECT_TRUE(run.lost);
    EXPECT_EQ(run.counts.verified, 2u);
    EXPECT_EQ(run.counts.mismatches, 2u);
}

// A producer filling a 1 KiB ring (it sleeps in Space when full), a consumer checking every
// packet and stalling for 1-2 ms (past the sleepers' spin, also on a loaded machine) every 1024
// packets, and three sleepers waiting for the whole queue or a point inside it until the
// consumer is done.
struct Stress {
    std::uint64_t sleeps = 0, verified = 0, mismatches = 0, lost = 0;
    bool intact = true;
};
Stress RunStress(RingWake mode) {
    ByteRing<1024> ring{mode};
    constexpr std::uint32_t Count = 100000;
    constexpr int Sleepers = 3;
    Stress result;
    std::atomic<std::uint64_t> lost{0};
    std::atomic<bool> intact{true}, consumed_all{false};
    FinishesWithin(std::chrono::seconds(120), [&] {
        std::thread consumer([&] {
            std::mt19937 rng{7};
            for (std::uint32_t i = 0; i < Count; ++i) {
                std::span<const std::byte> data;
                while ((data = ring.Peek()).empty()) {
                    _mm_pause();
                }
                std::uint32_t number;
                std::memcpy(&number, data.data(), sizeof(number));
                if (number != i || data.size() != 8 + (i * 37) % 232) {
                    intact = false;
                }
                if ((i & 1023) == 0) {
                    Stall(1000 + rng() % 1000);
                }
                ring.Release();
            }
            consumed_all = true;
        });
        std::vector<std::thread> sleepers;
        for (int s = 0; s < Sleepers; ++s) {
            sleepers.emplace_back([&, s] {
                std::mt19937 rng{static_cast<unsigned>(11 + s)};
                for (unsigned w = 0; !consumed_all; ++w) {
                    const std::uint64_t published = ring.Published();
                    const std::uint64_t back = rng() % 1024; // the ring's capacity
                    const bool found = (w & 1) ? ring.Drain()
                                               : ring.Wait(published > back ? published - back : 1);
                    lost += found;
                }
            });
        }
        for (std::uint32_t i = 0; i < Count; ++i) {
            const auto bytes = 8 + (i * 37) % 232;
            auto* data = ring.Begin(bytes);
            std::memset(data, i & 255, bytes);
            std::memcpy(data, &i, sizeof(i));
            ring.Commit();
            if (i % 4099 == 0) {
                lost += ring.Drain();
            }
        }
        lost += ring.Drain();
        for (auto& sleeper : sleepers) {
            sleeper.join();
        }
        consumer.join();
    });
    const auto counts = ring.Counts();
    result.sleeps = counts.sleeps;
    result.verified = counts.verified;
    result.mismatches = counts.mismatches;
    result.lost = lost;
    result.intact = intact && ring.Reached(ring.Published());
    return result;
}

// How many waits slept (on an idle machine about 400 per run; on a loaded one the sleepers' yields
// can outlast the stalls, so it is reported, not checked: the tests above sleep for certain).
void ReportSleeps(const char* mode, const Stress& run) {
    std::printf("  %s stress: %llu waits slept, %llu checked\n", mode,
                static_cast<unsigned long long>(run.sleeps),
                static_cast<unsigned long long>(run.verified));
}

TEST(RingWake, TargetLosesNoWakeUnderStress) {
    const auto run = RunStress(RingWake::Target);
    ReportSleeps("target", run);
    EXPECT_TRUE(run.intact);
    EXPECT_EQ(run.lost, 0u);
}

TEST(RingWake, VerifyFindsNoLostWakeUnderStress) {
    const auto run = RunStress(RingWake::Verify);
    ReportSleeps("verify", run);
    EXPECT_TRUE(run.intact);
    EXPECT_EQ(run.verified, run.sleeps); // every wait that slept was checked
    EXPECT_EQ(run.mismatches, 0u);
    EXPECT_EQ(run.lost, 0u);
}

TEST(RingWake, EveryPacketStillWorks) {
    // FAST_CPU_LAZY_WAKE=0: the previous protocol, uncounted.
    const auto run = RunStress(RingWake::EveryPacket);
    EXPECT_TRUE(run.intact);
    EXPECT_EQ(run.sleeps, 0u);
    EXPECT_EQ(run.lost, 0u);
}

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

TEST(RingWake, Switches) {
    SetVariable("FAST_CPU_LAZY_WAKE", nullptr);
    SetVariable("FAST_CPU_LAZY_WAKE_VERIFY", nullptr);
    EXPECT_TRUE(FastRender::RingWakeFromEnvironment() == RingWake::Target);
    SetVariable("FAST_CPU_LAZY_WAKE_VERIFY", "1");
    EXPECT_TRUE(FastRender::RingWakeFromEnvironment() == RingWake::Verify);
    SetVariable("FAST_CPU_LAZY_WAKE", "0");
    EXPECT_TRUE(FastRender::RingWakeFromEnvironment() == RingWake::EveryPacket);
    SetVariable("FAST_CPU_LAZY_WAKE", "1");
    SetVariable("FAST_CPU_LAZY_WAKE_VERIFY", "0");
    EXPECT_TRUE(FastRender::RingWakeFromEnvironment() == RingWake::Target);
    SetVariable("FAST_CPU_LAZY_WAKE", nullptr);
    SetVariable("FAST_CPU_LAZY_WAKE_VERIFY", nullptr);
}

TEST(BusyClock, WindowsAddUpToTheBusyTime) {
    // Busy from 1000 to 1000 + 150 * 10 (150 packets, then idle), and from 9000 for 5 packets.
    FastRender::BusyClock clock;
    std::uint64_t ticks = 0, packets = 0, closes = 0;
    const auto add = [&](const FastRender::BusyClock::Window& window) {
        ticks += window.ticks;
        packets += window.packets;
        ++closes;
        EXPECT_LE(window.timed, window.ticks);
    };
    std::uint64_t now = 1000;
    clock.Open(now);
    for (int i = 0; i < 150; ++i) {
        clock.timed += 7; // a handler's own 7 of each packet's 10 ticks
        now += 10;
        if (clock.Packet()) {
            add(clock.Close(now));
        }
    }
    add(clock.Close(now)); // idle
    EXPECT_EQ(closes, 3u); // full at 64 and 128, then the idle close
    now = 9000;
    clock.Open(now);
    for (int i = 0; i < 5; ++i) {
        now += 10;
        EXPECT_FALSE(clock.Packet());
    }
    add(clock.Close(now));
    EXPECT_EQ(ticks, 155u * 10u);
    EXPECT_EQ(packets, 155u);
}

} // namespace
