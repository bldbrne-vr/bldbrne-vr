// SPDX-License-Identifier: GPL-2.0-or-later

// The temporal provider's failure streak and S1's retry after it (FAST_GPU_S1_ASYM,
// src/video_core/renderer_vulkan/vr_temporal_failure.h): eight incomplete frames in a row latch
// the provider; S1's waits, idle, and retries, a retry that fails again within ProbeFailures
// frames doubles the wait up to MaxWait, and a reconstructed frame restores FirstWait. The run
// that showed it: S1 latched during the trial save's spawn animation (in the serial audit runs;
// the cause there is not settled) and never reconstructed again.

#include <cstdint>
#include <string>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vr_temporal_failure.h"

namespace {

using Vulkan::TemporalFailure;
using Vulkan::TemporalFailureFrames;
using Vulkan::TemporalRetry;

// VrTemporalBeforeDraw's frame start and VrS1Reconstruct's outcome, for one S1 frame whose scene
// is complete or not: 'i' idle (waiting for the retry), 'r' reconstructed, 's' skipped (the
// streak goes on), 'l' latched (idle from the next frame).
struct S1 {
    TemporalRetry retry;
    unsigned streak{};
    bool faulted{};
    uint64_t recovered{};
    char Frame(uint64_t frame, bool complete) {
        if (faulted && retry.Scheduled()) {
            if (retry.Waiting(frame))
                return 'i';
            faulted = false;
            retry.Start(streak);
        }
        if (complete) {
            streak = 0;
            recovered += retry.Success();
            return 'r';
        }
        if (TemporalFailure(streak)) {
            retry.Latch(frame);
            faulted = true;
            return 'l';
        }
        return 's';
    }
    std::string Run(uint64_t first, uint64_t last, bool complete) {
        std::string out;
        for (uint64_t frame = first; frame <= last; ++frame)
            out += Frame(frame, complete);
        return out;
    }
};

} // namespace

TEST(TemporalFailure, EightIncompleteFramesInARowLatch) {
    unsigned streak = 0;
    for (unsigned i = 1; i < TemporalFailureFrames; ++i)
        EXPECT_FALSE(TemporalFailure(streak)) << i;
    EXPECT_TRUE(TemporalFailure(streak));
    EXPECT_TRUE(TemporalFailure(streak)); // saturated, still latched
    EXPECT_EQ(streak, TemporalFailureFrames);
}

TEST(TemporalFailure, APermanentFailureLatchesAtOnce) {
    unsigned streak = 0;
    EXPECT_TRUE(TemporalFailure(streak, true));
    EXPECT_EQ(streak, 1u);
}

TEST(TemporalRetry, WaitsFirstWaitFramesAfterALatch) {
    TemporalRetry retry;
    EXPECT_FALSE(retry.Scheduled());
    EXPECT_FALSE(retry.Waiting(0));
    retry.Latch(100);
    EXPECT_TRUE(retry.Scheduled());
    EXPECT_TRUE(retry.Waiting(100));
    EXPECT_TRUE(retry.Waiting(100 + TemporalRetry::FirstWait - 1));
    EXPECT_FALSE(retry.Waiting(100 + TemporalRetry::FirstWait));
    EXPECT_EQ(retry.at, 100 + TemporalRetry::FirstWait);
}

TEST(TemporalRetry, AProbeLatchesAgainAfterProbeFailuresFrames) {
    TemporalRetry retry;
    unsigned streak = TemporalFailureFrames;
    retry.Latch(10);
    retry.Start(streak);
    EXPECT_FALSE(retry.Scheduled());
    EXPECT_TRUE(retry.probing);
    for (unsigned i = 1; i < TemporalRetry::ProbeFailures; ++i)
        EXPECT_FALSE(TemporalFailure(streak));
    EXPECT_TRUE(TemporalFailure(streak));
}

TEST(TemporalRetry, FailedProbesDoubleTheWaitUpToMaxWait) {
    TemporalRetry retry;
    unsigned streak = 0;
    uint64_t frame = 1000;
    retry.Latch(frame);
    EXPECT_EQ(retry.wait, TemporalRetry::FirstWait);
    uint32_t expected = TemporalRetry::FirstWait;
    for (int i = 0; i < 8; ++i) {
        frame = retry.at;
        retry.Start(streak);
        retry.Latch(frame + 1);
        expected = std::min(expected * 2, TemporalRetry::MaxWait);
        EXPECT_EQ(retry.wait, expected) << i;
        EXPECT_EQ(retry.at, frame + 1 + expected) << i;
    }
    EXPECT_EQ(retry.wait, TemporalRetry::MaxWait);
}

TEST(TemporalRetry, AReconstructedFrameEndsTheProbeAndRestoresTheWait) {
    TemporalRetry retry;
    unsigned streak = 0;
    retry.Latch(0);
    retry.Start(streak);
    retry.Latch(70); // the probe failed: 128
    retry.Start(streak);
    EXPECT_TRUE(retry.Success());
    EXPECT_FALSE(retry.probing);
    EXPECT_EQ(retry.wait, TemporalRetry::FirstWait);
    EXPECT_FALSE(retry.Success()); // ordinary frames
    // A later latch is not a failed probe: the first wait again.
    retry.Latch(500);
    EXPECT_EQ(retry.at, 500 + TemporalRetry::FirstWait);
}

TEST(TemporalRetry, ResetForgetsEverything) {
    TemporalRetry retry;
    unsigned streak = 0;
    retry.Latch(5);
    retry.Start(streak);
    retry.Latch(6);
    retry.Reset();
    EXPECT_FALSE(retry.Scheduled());
    EXPECT_FALSE(retry.probing);
    EXPECT_EQ(retry.wait, TemporalRetry::FirstWait);
}

// dlss_base_B: S1's first frames (752..759) fall in the spawn animation, all incomplete; the old
// latch kept the provider off for the rest of the run. With the retry it comes back once the
// scene is complete again.
TEST(TemporalRetry, TheTrialSaveSpawnAnimation) {
    S1 s1;
    EXPECT_EQ(s1.Run(752, 759, false), "sssssssl");
    EXPECT_EQ(s1.Run(760, 822, false), std::string(63, 'i'));
    // Still in the animation at the first retry: two frames, then twice the wait.
    EXPECT_EQ(s1.Run(823, 824, false), "sl");
    EXPECT_EQ(s1.retry.at, 824 + 2 * TemporalRetry::FirstWait);
    EXPECT_EQ(s1.Run(825, 951, true), std::string(127, 'i'));
    EXPECT_EQ(s1.Run(952, 960, true), std::string(9, 'r'));
    EXPECT_EQ(s1.recovered, 1u);
    EXPECT_EQ(s1.retry.wait, TemporalRetry::FirstWait);
    // An isolated incomplete frame later is skipped and does not latch.
    EXPECT_EQ(s1.Run(961, 961, false), "s");
    EXPECT_EQ(s1.Run(962, 962, true), "r");
}
