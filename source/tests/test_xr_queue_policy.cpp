// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenXR runtime's own queue and the mipped swapchains, by runtime
// (renderer_vulkan/vr_xr_queue_policy.h). The runtime names are the user's logs' (2026-10-15:
// "SteamVR/OpenXR 2.18.3", "Oculus 1.208.0") and the mock's ("MockXR"); the queue counts NVIDIA's
// graphics family (16) and a family with no spare queue.

#include <array>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_xr_queue_policy.h"

namespace {

namespace P = Vulkan::VrXrQueuePolicy;
using P::Runtime;

TEST(XrQueuePolicy, ClassifiesTheRuntimesOfTheLogs) {
    EXPECT_EQ(P::Classify("SteamVR/OpenXR"), Runtime::SteamVr);
    EXPECT_EQ(P::Classify("Oculus"), Runtime::Meta);
    EXPECT_EQ(P::Classify("Meta Quest Link"), Runtime::Meta);
    EXPECT_EQ(P::Classify("MockXR"), Runtime::Meta); // the mock runs Link's path
    EXPECT_EQ(P::Classify("VirtualDesktopXR"), Runtime::Other);
    EXPECT_EQ(P::Classify(""), Runtime::Other);
    // SteamVR's *system* name mentions its Oculus driver; its runtime name is SteamVR's.
    EXPECT_EQ(P::Classify("SteamVR/OpenXR : oculus"), Runtime::SteamVr);
}

TEST(XrQueuePolicy, OwnQueueByDefaultOnSteamVrAndOthersNotMeta) {
    // NVIDIA: 16 queues; the graphics queue and the desktop present's are taken.
    EXPECT_TRUE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 16, 2).own);
    EXPECT_TRUE(P::ChooseOwnQueue(nullptr, Runtime::Other, 16, 2).own);
    EXPECT_FALSE(P::ChooseOwnQueue(nullptr, Runtime::Meta, 16, 2).own);
    // An empty variable counts as unset.
    EXPECT_TRUE(P::ChooseOwnQueue("", Runtime::SteamVr, 16, 1).own);
    EXPECT_FALSE(P::ChooseOwnQueue("", Runtime::Meta, 16, 1).own);
}

TEST(XrQueuePolicy, ZeroIsTheSharedQueueOfBefore) {
    for (const Runtime r : {Runtime::SteamVr, Runtime::Meta, Runtime::Other}) {
        const auto choice = P::ChooseOwnQueue("0", r, 16, 2);
        EXPECT_FALSE(choice.own);
        EXPECT_EQ(choice.reason, "VR_XR_OWN_QUEUE is off");
    }
}

TEST(XrQueuePolicy, OffWordsAreTheSharedQueueToo) {
    // VR_XR_OWN_QUEUE=off / false / no (any case) no longer turn the own queue on.
    for (const char* off : {"off", "OFF", "Off", "false", "False", "no", "NO", "0", "00"}) {
        for (const Runtime r : {Runtime::SteamVr, Runtime::Other}) {
            EXPECT_FALSE(P::ChooseOwnQueue(off, r, 16, 2).own) << off;
        }
    }
    // Other words still ask for it (as any value but "0" did before).
    for (const char* on : {"1", "on", "yes", "true", "steam"}) {
        EXPECT_TRUE(P::ChooseOwnQueue(on, Runtime::Meta, 16, 2).own) << on;
    }
}

TEST(XrQueuePolicy, EnvSwitchWords) {
    EXPECT_FALSE(P::EnvOff(nullptr));
    EXPECT_FALSE(P::EnvOff(""));
    EXPECT_TRUE(P::EnvOff("0"));
    EXPECT_TRUE(P::EnvOff("fAlSe"));
    EXPECT_FALSE(P::EnvOff("offx")); // whole words only
    EXPECT_FALSE(P::EnvOff("of"));
    EXPECT_FALSE(P::EnvOff("1"));
    EXPECT_FALSE(P::EnvOn(nullptr));
    EXPECT_FALSE(P::EnvOn(""));
    EXPECT_TRUE(P::EnvOn("1"));
    EXPECT_TRUE(P::EnvOn("ON"));
    EXPECT_TRUE(P::EnvOn("Yes"));
    EXPECT_TRUE(P::EnvOn("true"));
    EXPECT_FALSE(P::EnvOn("0"));
    EXPECT_FALSE(P::EnvOn("tru"));
    EXPECT_FALSE(P::EnvOn("yess"));
}

TEST(XrQueuePolicy, OneForcesItOnMetaToo) {
    EXPECT_TRUE(P::ChooseOwnQueue("1", Runtime::Meta, 16, 2).own);
    EXPECT_TRUE(P::ChooseOwnQueue("1", Runtime::SteamVr, 16, 1).own);
}

TEST(XrQueuePolicy, NeedsASpareQueueInTheFamily) {
    // One queue: the graphics queue. Two: also the desktop present's. Never a queue past the count.
    EXPECT_FALSE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 1, 1).own);
    EXPECT_FALSE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 2, 2).own);
    EXPECT_FALSE(P::ChooseOwnQueue("1", Runtime::SteamVr, 2, 2).own);
    EXPECT_TRUE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 2, 1).own);
    EXPECT_TRUE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 3, 2).own);
    EXPECT_FALSE(P::ChooseOwnQueue(nullptr, Runtime::SteamVr, 0, 1).own);
}

TEST(XrQueuePolicy, MipsOnlyOnMeta) {
    // The plan's rule: mips only on the runtime known to handle them (and the mock, which stands in
    // for it); unknown runtimes (Virtual Desktop, WMR...) get one level, like SteamVR.
    EXPECT_FALSE(P::SwapchainMips(Runtime::SteamVr, nullptr, nullptr));
    EXPECT_TRUE(P::SwapchainMips(Runtime::Meta, nullptr, nullptr));
    EXPECT_FALSE(P::SwapchainMips(Runtime::Other, nullptr, nullptr));
    EXPECT_TRUE(P::SwapchainMips(P::Classify("MockXR"), nullptr, nullptr));
    EXPECT_FALSE(P::SwapchainMips(P::Classify("VirtualDesktopXR"), nullptr, nullptr));
    EXPECT_TRUE(P::SwapchainMips(P::Classify("Oculus"), nullptr, nullptr));
}

TEST(XrQueuePolicy, MipSwitches) {
    // VR_SWAPCHAIN_MIPS on: the mips back on any runtime for a test; other values change nothing.
    for (const Runtime r : {Runtime::SteamVr, Runtime::Other}) {
        EXPECT_TRUE(P::SwapchainMips(r, nullptr, "1"));
        EXPECT_TRUE(P::SwapchainMips(r, nullptr, "true"));
        EXPECT_FALSE(P::SwapchainMips(r, nullptr, "0"));
        EXPECT_FALSE(P::SwapchainMips(r, nullptr, ""));
    }
    EXPECT_TRUE(P::SwapchainMips(Runtime::Meta, nullptr, "0"));
    // VR_UI_NO_MIPS (set at all, as before): one level everywhere, whatever VR_SWAPCHAIN_MIPS says.
    for (const Runtime r : {Runtime::SteamVr, Runtime::Meta, Runtime::Other}) {
        EXPECT_FALSE(P::SwapchainMips(r, "1", nullptr));
        EXPECT_FALSE(P::SwapchainMips(r, "", "1"));
    }
}

TEST(XrQueuePolicy, GameQueueWaitsOnlyForWhatItHasNotSeen) {
    // Nothing to wait for: no value, or a value the queue already waits for.
    EXPECT_EQ(P::ChooseWait(5, 0, 0), P::Wait::None);
    EXPECT_EQ(P::ChooseWait(5, 5, 0), P::Wait::None);
    EXPECT_EQ(P::ChooseWait(5, 3, 0), P::Wait::None);
    // The host saw the XR queue past it: no submit.
    EXPECT_EQ(P::ChooseWait(5, 6, 6), P::Wait::Host);
    EXPECT_EQ(P::ChooseWait(0, 1, 9), P::Wait::Host);
    // Else an empty submit that waits.
    EXPECT_EQ(P::ChooseWait(5, 6, 5), P::Wait::Queue);
    EXPECT_EQ(P::ChooseWait(0, 1, 0), P::Wait::Queue);
}

TEST(XrQueuePolicy, XrQueueWaitsOncePerValue) {
    P::WaitedValues<2> w{};
    EXPECT_TRUE(w.Need(0xA, 10));
    EXPECT_FALSE(w.Need(0xA, 10)); // the same tick: one wait
    EXPECT_FALSE(w.Need(0xA, 9));  // an older one is covered
    EXPECT_TRUE(w.Need(0xA, 11));
    EXPECT_TRUE(w.Need(0xB, 3)); // another semaphore: its own value
    EXPECT_FALSE(w.Need(0xB, 3));
    EXPECT_FALSE(w.Need(0xA, 11));
    // No semaphore or no value: nothing.
    EXPECT_FALSE(w.Need(0, 4));
    EXPECT_FALSE(w.Need(0xC, 0));
}

TEST(XrQueuePolicy, AFullTableWaitsTooOftenNeverTooLittle) {
    P::WaitedValues<2> w{};
    EXPECT_TRUE(w.Need(0xA, 10));
    EXPECT_TRUE(w.Need(0xB, 10));
    EXPECT_TRUE(w.Need(0xC, 10)); // forgets A
    EXPECT_TRUE(w.Need(0xA, 10)); // waited again (harmless), forgets B
    EXPECT_FALSE(w.Need(0xC, 10));
    EXPECT_FALSE(w.Need(0xA, 10));
    EXPECT_TRUE(w.Need(0xB, 10));
}

// A model of the XR queue's work: each acquire submits some (its index), each signal comes after
// everything before it (its value: the count so far).
struct XrQueueModel {
    u64 submitted{0};
    u32 signals{0};
    u64 Acquire() {
        return ++submitted;
    }
    u64 Signal() {
        ++signals;
        return ++submitted;
    }
};

TEST(XrQueuePolicy, OneSignalForTheImagesOfAnAdoption) {
    // ReleaseAdopted: the two eyes, their depth and a UI chain acquired again; one XR-queue signal
    // after the last, its value for all five (the graphics queue waits past each one's acquire).
    XrQueueModel xr{};
    const auto signal = [&xr] { return xr.Signal(); };
    std::array<u64, 5> acquire_value{};
    std::array<u64, 5> acquired_at{};
    P::SignalBatch<8> batch{};
    for (u32 i = 0; i < 5; ++i) {
        acquired_at[i] = xr.Acquire();
        batch.Add(acquire_value[i], signal);
    }
    EXPECT_EQ(xr.signals, 0u); // nothing before the batch ends
    batch.Flush(signal);
    EXPECT_EQ(xr.signals, 1u); // was 5: one empty vkQueueSubmit per image
    for (u32 i = 0; i < 5; ++i) {
        EXPECT_GT(acquire_value[i], acquired_at[i]);
        EXPECT_EQ(acquire_value[i], acquire_value[0]);
    }
    batch.Flush(signal); // nothing left: no signal
    EXPECT_EQ(xr.signals, 1u);
}

TEST(XrQueuePolicy, AFullSignalBatchSignalsEarlyNeverLate) {
    XrQueueModel xr{};
    const auto signal = [&xr] { return xr.Signal(); };
    std::array<u64, 5> acquire_value{};
    std::array<u64, 5> acquired_at{};
    P::SignalBatch<2> batch{};
    for (u32 i = 0; i < 5; ++i) {
        acquired_at[i] = xr.Acquire();
        batch.Add(acquire_value[i], signal);
    }
    batch.Flush(signal);
    EXPECT_EQ(xr.signals, 3u); // 2 + 2 + 1
    for (u32 i = 0; i < 5; ++i) {
        EXPECT_GT(acquire_value[i], acquired_at[i]) << i; // never an image without its signal
    }
    // A signal that failed (0: no ordering, as an unbatched failed signal) reaches every image.
    P::SignalBatch<8> failed{};
    u64 a = 7;
    u64 b = 9;
    failed.Add(a, [] { return u64{0}; });
    failed.Add(b, [] { return u64{0}; });
    failed.Flush([] { return u64{0}; });
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 0u);
}

} // namespace
