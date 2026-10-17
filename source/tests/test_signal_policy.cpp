// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_ASYNC_IDLE and FAST_CPU_SIGNAL_AT_REPLAY (vk_signal_policy.h in
// video_core/renderer_vulkan): a model of the producer, the recorder's FIFO replay and the guest
// signals, driven by random operations. A sync point that submits when SignalPolicy::NeedsFlush
// says so and then drains must leave no signal unwritten in every mode; Old and Precise never
// write a signal encoded since the last submission (SubmitOrderHolds); and the async retirement's
// count of the last outstanding submission makes the same decisions as the old path's synchronous
// one, with num_submits reaching zero only at a retirement the recorder replays.

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_signal_policy.h"

namespace {

namespace SP = Vulkan::SignalPolicy;

TEST(SignalPolicy, NeedsFlushByMode) {
    // Nothing pending.
    EXPECT_FALSE(SP::NeedsFlush(SP::Mode::Old, {5, 5, 5}));
    EXPECT_FALSE(SP::NeedsFlush(SP::Mode::Precise, {5, 5, 5}));
    EXPECT_FALSE(SP::NeedsFlush(SP::Mode::AtReplay, {5, 5, 5}));
    // Pending, a submission encoded after them still on its way: only Old submits again.
    EXPECT_TRUE(SP::NeedsFlush(SP::Mode::Old, {5, 3, 5}));
    EXPECT_FALSE(SP::NeedsFlush(SP::Mode::Precise, {5, 3, 5}));
    EXPECT_FALSE(SP::NeedsFlush(SP::Mode::AtReplay, {5, 3, 5}));
    // Encoded since the last submission: every mode submits.
    EXPECT_TRUE(SP::NeedsFlush(SP::Mode::Old, {7, 3, 5}));
    EXPECT_TRUE(SP::NeedsFlush(SP::Mode::Precise, {7, 3, 5}));
    EXPECT_TRUE(SP::NeedsFlush(SP::Mode::AtReplay, {7, 3, 5}));
    // At replay, written already although no submission followed them: the work before them is
    // still submitted at a sync point (as the old path did).
    EXPECT_TRUE(SP::NeedsFlush(SP::Mode::AtReplay, {7, 7, 5}));
    EXPECT_FALSE(SP::SubmitOrderHolds({7, 7, 5}));
    EXPECT_TRUE(SP::SubmitOrderHolds({7, 6, 5}));
    EXPECT_TRUE(SP::SubmitOrderHolds({5, 5, 5}));
}

TEST(SignalPolicy, LastSubmissionSubtractsQueuedRetirements) {
    EXPECT_TRUE(SP::LastSubmission(1, 0));
    EXPECT_FALSE(SP::LastSubmission(2, 0));
    EXPECT_TRUE(SP::LastSubmission(2, 1));
    EXPECT_TRUE(SP::LastSubmission(3, 2));
    EXPECT_FALSE(SP::LastSubmission(3, 1));
}

// The producer, the replay FIFO and the guest memory, for one mode.
struct Model {
    enum class Kind { Signal, Submit };
    struct Packet {
        Kind kind;
        std::uint64_t sequence;
    };
    explicit Model(SP::Mode mode_) : mode{mode_} {}
    SP::Mode mode;
    std::uint64_t encoded{}, at_submit{}, completed{}, submits{};
    bool in_order{true}; // every signal written once, in sequence
    std::deque<Packet> ring;
    std::vector<std::uint64_t> deferred; // replay owner: signals waiting for a submission
    SP::Counts Counts() const {
        return {encoded, completed, at_submit};
    }
    void EncodeSignal() {
        ring.push_back({Kind::Signal, ++encoded});
    }
    void EncodeSubmit() {
        at_submit = encoded;
        ++submits;
        ring.push_back({Kind::Submit, 0});
    }
    void ReplayOne() {
        if (ring.empty())
            return;
        const Packet p = ring.front();
        ring.pop_front();
        if (p.kind == Kind::Signal) {
            if (mode == SP::Mode::AtReplay) {
                in_order &= p.sequence == completed + 1; // FIFO: in order, none skipped
                completed = p.sequence;
            } else {
                deferred.push_back(p.sequence);
            }
        } else {
            for (const std::uint64_t s : deferred) {
                in_order &= s == completed + 1;
                completed = s;
            }
            deferred.clear();
        }
    }
    void Drain() {
        while (!ring.empty())
            ReplayOne();
    }
    // SyncRecorder / FlushGuestSignals: submit when the policy says so, then drain. False when
    // a signal stayed unwritten.
    bool SyncPoint() {
        if (SP::NeedsFlush(mode, Counts()))
            EncodeSubmit();
        Drain();
        return !SP::Pending(Counts());
    }
    // FAST_CPU_SIGNAL_AT_REPLAY's WaitGuestSignals: replay until the newest signal is written,
    // without a submission (the commands after it may stay queued). False if the ring ran dry
    // first.
    bool WaitSignals() {
        const std::uint64_t target = encoded;
        while (completed < target) {
            if (ring.empty())
                return false;
            ReplayOne();
        }
        return true;
    }
};

// The first operation that broke a property, or -1.
int RunSignals(SP::Mode mode, std::uint32_t seed) {
    Model m{mode};
    std::mt19937 rng{seed};
    std::uniform_int_distribution<int> op{0, 99};
    for (int i = 0; i < 20000; ++i) {
        const int o = op(rng);
        bool ok = true;
        if (o < 25)
            m.EncodeSignal();
        else if (o < 35)
            m.EncodeSubmit(); // the cadence or another flush
        else if (o < 80)
            m.ReplayOne();
        else if (o < 90 || mode != SP::Mode::AtReplay)
            ok = m.SyncPoint();
        else
            ok = m.WaitSignals();
        if (mode != SP::Mode::AtReplay)
            ok = ok && SP::SubmitOrderHolds(m.Counts());
        if (!ok || !m.in_order)
            return i;
    }
    return -1;
}

TEST(SignalPolicy, SyncPointsWriteEverySignalOld) {
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
        EXPECT_EQ(RunSignals(SP::Mode::Old, seed), -1) << "seed " << seed;
}
TEST(SignalPolicy, SyncPointsWriteEverySignalPrecise) {
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
        EXPECT_EQ(RunSignals(SP::Mode::Precise, seed), -1) << "seed " << seed;
}
TEST(SignalPolicy, SyncPointsWriteEverySignalAtReplay) {
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
        EXPECT_EQ(RunSignals(SP::Mode::AtReplay, seed), -1) << "seed " << seed;
}

// Precise leaves out exactly the submissions Old makes while an earlier one is still queued:
// starting from the same state, a sync point right after a submission whose replay has not run.
TEST(SignalPolicy, PreciseSkipsTheSecondSubmission) {
    for (const SP::Mode mode : {SP::Mode::Old, SP::Mode::Precise}) {
        Model m{mode};
        m.EncodeSignal();
        m.EncodeSignal();
        m.EncodeSubmit(); // the frame end's submission, not replayed yet
        EXPECT_TRUE(m.SyncPoint()); // the GPU-idle task, or the next submission's first WriteData
        EXPECT_EQ(m.submits, mode == SP::Mode::Old ? 2u : 1u);
        EXPECT_EQ(m.completed, 2u);
    }
}

// Liverpool's retirement: guest submissions, B's in-order completions and the recorder's queued
// retirements, against the old path's synchronous decrement, from the same random order. Returns
// the first operation that broke a property, or -1; `lasts` counts the async retirements.
int RunRetirements(std::uint32_t seed, std::uint64_t& lasts) {
    std::mt19937 rng{seed};
    std::uniform_int_distribution<int> op{0, 99};
    std::uint32_t num_async{}, async_retires{}, num_old{};
    std::uint64_t submitted{}, completed_on_b{};
    std::deque<int> retire_queue; // the recorder's FIFO of queued retirements
    for (int i = 0; i < 50000; ++i) {
        const int o = op(rng);
        if (o < 30) {
            if (submitted - completed_on_b < 4) { // the guest submits
                ++num_async;
                ++num_old;
                ++submitted;
            }
        } else if (o < 65) {
            if (completed_on_b < submitted) { // B retires the oldest submission
                ++completed_on_b;
                const bool last_old = num_old == 1;
                --num_old;
                const bool last = SP::LastSubmission(num_async, async_retires);
                if (last != last_old)
                    return i;
                if (last) {
                    ++lasts;
                    ++async_retires;
                    retire_queue.push_back(0);
                } else if (--num_async == 0) {
                    return i; // only a replayed retirement may reach zero
                }
            }
        } else if (!retire_queue.empty()) { // the recorder reaches a retirement
            retire_queue.pop_front();
            --num_async;
            --async_retires;
        }
        if (async_retires != retire_queue.size() || num_old != submitted - completed_on_b ||
            num_async != num_old + async_retires)
            return i;
    }
    return -1;
}

TEST(SignalPolicy, AsyncRetirementMatchesTheOldDecisions) {
    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        std::uint64_t lasts = 0;
        EXPECT_EQ(RunRetirements(seed, lasts), -1) << "seed " << seed;
        EXPECT_GT(lasts, 0u) << "seed " << seed;
    }
}

} // namespace
