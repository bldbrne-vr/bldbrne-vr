// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini stereo_adopt_finished (renderer_vulkan/vr_stereo_queue.h): which published DIBR pair goes
// up and which pair a new frame is copied into while the GPU runs behind. The direct cases, the
// old behaviour at 0, and a simulation of the presenter's loop (a game frame every 2 refreshes of
// 120 Hz, a minimum hold of 2) with the GPU a fixed or a draining number of refreshes behind.

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_stereo_queue.h"

namespace Vulkan::VrStereoQueue {
namespace {

std::vector<Entry> Q(std::initializer_list<Entry> e) {
    return std::vector<Entry>(e);
}

TEST(StereoQueue, SettingMapsToWaitingPairsAndSlots) {
    EXPECT_EQ(Waiting(0), 1u);
    EXPECT_EQ(Waiting(-3), 1u);
    EXPECT_EQ(Waiting(1), 2u);
    EXPECT_EQ(Waiting(2), 3u);
    EXPECT_EQ(Waiting(9), MaxWaiting);
    EXPECT_EQ(Pairs(Waiting(0)), 2u); // as before: two pairs
    EXPECT_EQ(LateSlots(Waiting(0)), 3u); // as before: three capture slots
    EXPECT_EQ(Pairs(Waiting(1)), 3u);
    EXPECT_EQ(LateSlots(Waiting(1)), 4u);
    EXPECT_LE(LateSlots(MaxWaiting), 5u); // VrStereoWarp::LateSlots
    EXPECT_EQ(Pairs(MaxWaiting), MaxPairs);
}

TEST(StereoQueue, AdoptTheNewestFinished) {
    EXPECT_EQ(ChooseAdopt({}), -1);
    auto a = Q({{1, 10, false}});
    EXPECT_EQ(ChooseAdopt(a), -1);
    a = Q({{1, 10, true}});
    EXPECT_EQ(ChooseAdopt(a), 0);
    a = Q({{1, 10, true}, {2, 11, false}});
    EXPECT_EQ(ChooseAdopt(a), 0); // the older one goes up, the newer keeps waiting
    a = Q({{1, 10, true}, {2, 11, true}});
    EXPECT_EQ(ChooseAdopt(a), 1); // both done: the newer, the older is dropped
    a = Q({{1, 10, false}, {2, 11, false}});
    EXPECT_EQ(ChooseAdopt(a), -1);
}

TEST(StereoQueue, WaitForTheOldestUnlessOneIsDone) {
    EXPECT_EQ(ChooseWait({}), -1);
    auto a = Q({{1, 10, false}, {2, 11, false}});
    EXPECT_EQ(ChooseWait(a), 0);
    a = Q({{1, 10, true}, {2, 11, false}});
    EXPECT_EQ(ChooseWait(a), -1);
    a = Q({{1, 10, false}});
    EXPECT_EQ(ChooseWait(a), 0);
}

TEST(StereoQueue, OneWaitingPairIsTheOldBehaviour) {
    // The pair not on display; with none shown pair 0; a waiting frame there is replaced.
    EXPECT_EQ(ChooseRecordPair(0, 1, {}).pair, 1u);
    EXPECT_EQ(ChooseRecordPair(1, 1, {}).pair, 0u);
    EXPECT_EQ(ChooseRecordPair(-1, 1, {}).pair, 0u);
    EXPECT_EQ(ChooseRecordPair(0, 1, {}).drop, -1);
    for (const bool done : {false, true}) {
        const auto q = Q({{1, 7, done}});
        const RecordChoice c = ChooseRecordPair(0, 1, q);
        EXPECT_EQ(c.pair, 1u);
        EXPECT_EQ(c.drop, 0);
    }
}

TEST(StereoQueue, TwoWaitingPairsKeepTheOneThatGoesUpFirst) {
    // Room: a pair neither shown nor waiting, nothing dropped.
    auto q = Q({{1, 7, false}});
    RecordChoice c = ChooseRecordPair(0, 2, q);
    EXPECT_EQ(c.pair, 2u);
    EXPECT_EQ(c.drop, -1);
    q = Q({{2, 7, false}});
    c = ChooseRecordPair(0, 2, q);
    EXPECT_EQ(c.pair, 1u);
    EXPECT_EQ(c.drop, -1);
    // Full, neither done: the newer gives way (the older finishes first).
    q = Q({{1, 7, false}, {2, 8, false}});
    c = ChooseRecordPair(0, 2, q);
    EXPECT_EQ(c.drop, 1);
    EXPECT_EQ(c.pair, 2u);
    // Full, the older done (held for the cadence): it stays, the newer gives way.
    q = Q({{1, 7, true}, {2, 8, false}});
    c = ChooseRecordPair(0, 2, q);
    EXPECT_EQ(c.drop, 1);
    EXPECT_EQ(c.pair, 2u);
    // Full, the newer done: the older would never go up, it gives way.
    q = Q({{1, 7, true}, {2, 8, true}});
    c = ChooseRecordPair(0, 2, q);
    EXPECT_EQ(c.drop, 0);
    EXPECT_EQ(c.pair, 1u);
    // None shown (the menu panel): still bounded.
    q = Q({{0, 7, false}, {1, 8, false}});
    c = ChooseRecordPair(-1, 2, q);
    EXPECT_EQ(c.drop, 1);
    EXPECT_EQ(c.pair, 1u);
}

// The presenter's loop: every refresh the frame thread adopts (after the minimum hold); every
// `game` refreshes the GPU thread records and publishes a frame that the GPU finishes `lag(k)`
// refreshes later (in draw order). Returns, per refresh, the frame on display (-1: none yet).
struct SimResult {
    std::vector<s64> shown;
    u32 never_shown{};
    u32 longest_hold{}; // refreshes one frame stayed up (after the first)
    std::vector<std::pair<u32, u32>> holds; // (refresh it was replaced at, refreshes it was up)
    u32 Longest(u32 t0, u32 t1) const {
        u32 out = 0;
        for (const auto& [t, h] : holds) {
            out = (t >= t0 && t < t1) ? std::max(out, h) : out;
        }
        return out;
    }
};

template <typename Lag>
SimResult Simulate(u32 waiting, u32 refreshes, u32 game, Lag&& lag) {
    struct W {
        Entry e;
        s64 done_at;
    };
    std::vector<W> queue;
    s32 shown_pair = -1;
    s64 shown_frame = -1;
    u32 held = 0;
    s64 last_done = -1;
    u32 published = 0;
    std::vector<bool> was_shown;
    SimResult r;
    u32 hold = 0;
    for (u32 t = 0; t < refreshes; ++t) {
        // GPU thread: a new frame (its pair chosen, then published).
        if (t % game == 0) {
            const u64 seq = published++;
            const s64 done_at = std::max<s64>(t + lag(seq), last_done); // in draw order
            last_done = done_at;
            std::vector<Entry> e;
            for (auto& w : queue) {
                w.e.done = w.done_at <= t;
                e.push_back(w.e);
            }
            const RecordChoice c = ChooseRecordPair(shown_pair, waiting, e);
            EXPECT_NE(static_cast<s32>(c.pair), shown_pair);
            if (c.drop >= 0) {
                queue.erase(queue.begin() + c.drop);
            }
            for (const auto& w : queue) {
                EXPECT_NE(w.e.pair, c.pair); // never a waiting pair's swapchain
            }
            EXPECT_LT(c.pair, Pairs(waiting));
            queue.push_back({{c.pair, seq, false}, done_at});
            EXPECT_LE(queue.size(), waiting);
            was_shown.push_back(false);
        }
        // Frame thread: the shown pair's hold, then the newest finished waiting pair.
        if (shown_pair < 0 || held >= 2) {
            std::vector<Entry> e;
            for (auto& w : queue) {
                w.e.done = w.done_at <= t;
                e.push_back(w.e);
            }
            const s32 pick = ChooseAdopt(e);
            if (pick >= 0) {
                if (shown_frame >= 0) {
                    r.longest_hold = std::max(r.longest_hold, hold);
                    r.holds.emplace_back(t, hold);
                }
                shown_pair = static_cast<s32>(queue[pick].e.pair);
                EXPECT_GT(static_cast<s64>(queue[pick].e.seq), shown_frame); // never backwards
                shown_frame = static_cast<s64>(queue[pick].e.seq);
                was_shown[shown_frame] = true;
                queue.erase(queue.begin(), queue.begin() + pick + 1);
                held = 0;
                hold = 0;
            }
        }
        r.shown.push_back(shown_frame);
        ++held;
        ++hold;
    }
    // The last few frames may still be waiting at the end.
    for (u32 i = 0; i + 4 < was_shown.size(); ++i) {
        r.never_shown += was_shown[i] ? 0 : 1;
    }
    return r;
}

TEST(StereoQueue, OnTimeGpuShowsEveryFrameEitherWay) {
    for (const u32 waiting : {1u, 2u, 3u}) {
        const SimResult r = Simulate(waiting, 600, 2, [](u64) { return 1; });
        EXPECT_EQ(r.never_shown, 0u) << waiting;
        EXPECT_EQ(r.longest_hold, 2u) << waiting;
    }
}

TEST(StereoQueue, GpuAFrameBehindFreezesWithOneButNotWithTwo) {
    // The GPU finishes each frame 3 refreshes after it is published: more than a game frame (2).
    const SimResult old = Simulate(1, 600, 2, [](u64) { return 3; });
    // Every frame is replaced before it is finished: nothing ever goes up.
    EXPECT_EQ(old.shown.back(), -1);
    const SimResult fix = Simulate(2, 600, 2, [](u64) { return 3; });
    EXPECT_EQ(fix.never_shown, 0u);
    EXPECT_EQ(fix.longest_hold, 2u);
    EXPECT_GE(fix.shown.back(), 290);
}

TEST(StereoQueue, GpuTwoAndAHalfFramesBehind) {
    // Frames 50-109 finish 5 refreshes after they are published (2.5 game frames, 42 ms).
    const auto lag = [](u64 k) { return (k >= 50 && k < 110) ? s64{5} : s64{1}; };
    const SimResult old = Simulate(1, 600, 2, lag);
    const SimResult fix = Simulate(2, 600, 2, lag);
    const SimResult fix3 = Simulate(3, 600, 2, lag);
    EXPECT_GE(old.longest_hold, 110u); // the whole stretch stands still
    EXPECT_GE(old.never_shown, 55u);
    // Into the stretch the GPU finishes nothing for 6 refreshes (one hold of 6 either way); in it
    // (frames 55-105) the picture moves on every other frame with two waiting (30 images a
    // second) and with every frame with three.
    EXPECT_LE(fix.longest_hold, 6u);
    EXPECT_LE(fix.Longest(120, 210), 4u);
    EXPECT_LE(fix.never_shown, 32u);
    EXPECT_LE(fix3.longest_hold, 6u);
    EXPECT_LE(fix3.Longest(120, 210), 2u);
    EXPECT_LE(fix3.never_shown, 2u);
}

TEST(StereoQueue, BacklogDrainsWithoutAFreeze) {
    // A stutter leaves the GPU 40 refreshes (333 ms) behind; it then catches up by a quarter of a
    // refresh per game frame (3200 in the clinic: about 2.6 ms of 16.7 spare).
    const auto lag = [](u64 k) {
        if (k < 50) {
            return s64{1};
        }
        const s64 behind = 40 - static_cast<s64>((k - 50) / 4);
        return std::max<s64>(behind, 1);
    };
    const SimResult old = Simulate(1, 900, 2, lag);
    const SimResult fix = Simulate(2, 900, 2, lag);
    const SimResult fix3 = Simulate(3, 900, 2, lag);
    // Before: the picture stands still for most of the drain (each frame replaced unfinished).
    EXPECT_GE(old.longest_hold, 100u);
    // With two waiting: it keeps moving; the longest hold is the stutter itself (the GPU finishes
    // nothing for 40 refreshes) plus at most a game frame.
    EXPECT_LE(fix.longest_hold, 42u);
    EXPECT_LE(fix3.longest_hold, 42u);
    EXPECT_LT(fix.never_shown, old.never_shown);
    EXPECT_LE(fix3.never_shown, fix.never_shown);
}

TEST(StereoQueue, ShownFrameNeverGoesBackwardsUnderRandomLag) {
    u64 rng = 12345;
    const auto next = [&rng] {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<s64>((rng >> 33) % 7);
    };
    std::vector<s64> lags(2000);
    for (auto& l : lags) {
        l = 1 + next();
    }
    for (const u32 waiting : {1u, 2u, 3u}) {
        // (the monotonic and pair checks are inside Simulate)
        const SimResult r = Simulate(waiting, 3000, 2, [&](u64 k) { return lags[k % lags.size()]; });
        EXPECT_GE(r.shown.back(), 0) << waiting;
    }
}

TEST(StereoQueue, UiPoolProtectsShownQueuedAndAcquiredImages) {
    std::array<bool, MaxUiChains> acquired{};
    const std::array<s32, 1> queued{1};
    acquired[2] = true; // a concurrent present-thread copy
    EXPECT_EQ(ChooseUiChain(MaxUiChains, 0, queued, acquired), 3);
    EXPECT_EQ(ChooseUiChain(3, 0, queued, acquired), -1);
    EXPECT_EQ(ChooseUiChain(0, -1, {}, acquired), -1);
    EXPECT_EQ(ChooseUiChain(4, -1, {}, {}), -1);
    acquired[2] = false; // failed/released copy no longer owns it
    EXPECT_EQ(ChooseUiChain(3, 0, queued, acquired), 2);
    EXPECT_EQ(UiChains(0), 2u);
    EXPECT_EQ(UiChains(MaxWaiting + 5), MaxUiChains);
}

TEST(StereoQueue, UiImageCanBeReusedAfterQueueEvictionOrClear) {
    std::array<bool, MaxUiChains> acquired{};
    std::vector<s32> queued{1, 2};
    EXPECT_EQ(ChooseUiChain(3, 0, queued, acquired), -1);
    queued.erase(queued.begin()); // evicted eye frame gives up its UI too
    EXPECT_EQ(ChooseUiChain(3, 0, queued, acquired), 1);
    queued.clear(); // mode switch / swapchain recreation
    EXPECT_EQ(ChooseUiChain(3, 0, queued, acquired), 1);
    // A frame with no new UI retains the shown UI, independently of its eye-pair number.
    EXPECT_EQ(ChooseUiChain(3, 2, queued, acquired), 0);
}

TEST(StereoQueue, UiAdvancesWithCompletedWorldWhileNewerFrameIsStillRendering) {
    // Reproduce sustained GPU backlog: every new publication precedes adoption of an older
    // completed frame. The old two-chain allocator reused chain 1 on each publication and
    // PublishStereo invalidated the older frame's UI, leaving the menu frozen on frame 0.
    for (u32 waiting = 2; waiting <= MaxWaiting; ++waiting) {
        struct Frame {
            Entry eye;
            s32 ui;
        };
        std::vector<Frame> queue;
        std::array<bool, MaxUiChains> acquired{};
        std::array<u64, MaxUiChains> pixels{};
        s32 shown_ui = 0;
        u32 shown_pair = 0;
        u64 shown_seq = 0;
        for (u64 seq = 1; seq <= 1000; ++seq) {
            std::vector<Entry> eyes;
            for (const auto& frame : queue) {
                eyes.push_back(frame.eye);
            }
            const auto choice = ChooseRecordPair(shown_pair, waiting, eyes);
            if (choice.drop >= 0) {
                queue.erase(queue.begin() + choice.drop);
            }
            std::vector<s32> reserved;
            for (const auto& frame : queue) {
                reserved.push_back(frame.ui);
            }
            const s32 ui = ChooseUiChain(UiChains(waiting), shown_ui, reserved, acquired);
            ASSERT_GE(ui, 0);
            EXPECT_NE(ui, shown_ui);
            for (const auto& frame : queue) {
                ASSERT_NE(ui, frame.ui); // overwriting this chain recreates the freeze
            }
            pixels[ui] = seq;
            queue.push_back({{choice.pair, seq, false}, ui});
            // Only the oldest frame has finished; the newest must keep its UI for later.
            if (queue.size() >= waiting) {
                queue.front().eye.done = true;
                eyes.clear();
                for (const auto& frame : queue) {
                    eyes.push_back(frame.eye);
                }
                const s32 pick = ChooseAdopt(eyes);
                ASSERT_EQ(pick, 0);
                const auto frame = queue[pick];
                EXPECT_GT(frame.eye.seq, shown_seq);
                EXPECT_EQ(pixels[frame.ui], frame.eye.seq);
                shown_seq = frame.eye.seq;
                shown_ui = frame.ui;
                shown_pair = frame.eye.pair;
                queue.erase(queue.begin(), queue.begin() + pick + 1);
            }
        }
        EXPECT_EQ(shown_seq, 1001u - waiting);
    }
}

} // namespace
} // namespace Vulkan::VrStereoQueue
