// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Which queue the OpenXR runtime gets and whether its swapchains may have mip levels, by runtime.
// Header-only (tests/test_xr_queue_policy.cpp); used by Instance::CreateDevice (the queue),
// OpenXRPresenter (the UI layer's chains) and VrPageLayer (the VR page), and the bookkeeping
// VrXrQueue (vr_xr_queue.h) runs on.
//
// XR_KHR_vulkan_enable: the runtime may submit on the queue named in XrGraphicsBindingVulkanKHR
// inside xrBeginFrame, xrEndFrame, xrAcquireSwapchainImage and xrReleaseSwapchainImage, so the
// application must hold that queue's lock across them. Until 2026-10-15 the session had the
// graphics queue (queue 0, the game's), so those calls held Scheduler::submit_mutex. On Meta's
// runtime xrEndFrame takes 0.3 ms; SteamVR's takes 9.6-10.8 ms of every 11.1 ms refresh (the
// user's Quest 3 through SteamVR, 2026-10-15), so the game's submits waited behind it and the game
// ran at 28-31 frames/s with its GPU 83% idle. With a queue of its own (a spare queue of the
// graphics family) the runtime's calls hold only that queue's lock, and the game's submits never
// wait for them.
//
// SteamVR also copies every layer's swapchain image into its own D3D texture. With mip levels on
// the UI and page swapchains its compositor's AcquireSync timed out, the flat screen went white and
// the GPU hung (TDR, device lost) within a minute, three times out of three; one level fixed both
// (VR_UI_NO_MIPS=1). The other Bloodborne VR port, which runs on SteamVR by default, makes every
// swapchain with one level (bbport gpu/xr_bridge/bbport_xr_bridge.cpp CreateChain). So the mips
// stay only on the runtime known to handle them (Meta's); every other runtime gets one level.

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "common/types.h"

namespace Vulkan::VrXrQueuePolicy {

enum class Runtime : u8 {
    Other,   // Virtual Desktop, WMR, Pimax, Monado...
    SteamVr, // XrInstanceProperties::runtimeName "SteamVR/OpenXR"
    Meta,    // "Oculus": Meta's PC runtime (Link, Air Link); also "MockXR", below
};

constexpr Runtime Classify(std::string_view runtime_name) {
    if (runtime_name.find("SteamVR") != std::string_view::npos) {
        return Runtime::SteamVr;
    }
    // MockXR (the test harness, the test harness) stands in for a Quest 3 on Link: classed as
    // Meta's, the mock runs Link's path (the shared queue, mip chains) so its timings stay
    // comparable with the headset's. VR_XR_OWN_QUEUE=1 with VR_UI_NO_MIPS=1 runs SteamVR's there.
    if (runtime_name.find("Oculus") != std::string_view::npos ||
        runtime_name.starts_with("Meta") || runtime_name.starts_with("MockXR")) {
        return Runtime::Meta;
    }
    return Runtime::Other;
}

constexpr std::string_view Name(Runtime runtime) {
    switch (runtime) {
    case Runtime::SteamVr:
        return "SteamVR";
    case Runtime::Meta:
        return "Meta";
    default:
        return "other";
    }
}

// An environment switch's value (`env`; nullptr: unset) is one of `words` (ASCII, lower case),
// in any case.
constexpr bool EnvIsWord(const char* env, std::initializer_list<std::string_view> words) {
    if (env == nullptr) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (const std::string_view word : words) {
        std::size_t i = 0;
        while (i < word.size() && env[i] != '\0' && lower(env[i]) == word[i]) {
            ++i;
        }
        if (i == word.size() && env[i] == '\0') {
            return true;
        }
    }
    return false;
}

// A switch turned off: a value starting with '0' (as before), or "off", "false" or "no".
constexpr bool EnvOff(const char* env) {
    return env != nullptr && (env[0] == '0' || EnvIsWord(env, {"off", "false", "no"}));
}

// A switch turned on: a value starting with '1' (as before), or "on", "true" or "yes".
constexpr bool EnvOn(const char* env) {
    return env != nullptr && (env[0] == '1' || EnvIsWord(env, {"on", "true", "yes"}));
}

// VR_XR_OWN_QUEUE (`env`, nullptr when unset): off (EnvOff: "0", "off", "false", "no") the
// runtime shares the graphics queue (the path before 2026-10-15), any other value a queue of its
// own when the graphics family has a spare one; unset (or empty) a queue of its own except on
// Meta's runtime, which keeps the shared queue it has always run on. `family_queues`: the graphics
// family's queueCount; `queues_used`: the queues already taken from it (the graphics queue, and
// the desktop present's own).
struct OwnQueue {
    bool own{false};
    std::string_view reason{};
};

constexpr OwnQueue ChooseOwnQueue(const char* env, Runtime runtime, u32 family_queues,
                                  u32 queues_used) {
    const bool set = env != nullptr && env[0] != '\0';
    if (EnvOff(env)) {
        return {false, "VR_XR_OWN_QUEUE is off"};
    }
    if (family_queues <= queues_used) {
        return {false, "the graphics queue family has no spare queue"};
    }
    if (set) {
        return {true, "VR_XR_OWN_QUEUE"};
    }
    if (runtime == Runtime::Meta) {
        return {false, "Meta's runtime (its xrEndFrame is quick): the shared queue, as before"};
    }
    return {true, runtime == Runtime::SteamVr
                      ? "SteamVR (its xrEndFrame takes most of a refresh)"
                      : "the default for runtimes other than Meta's"};
}

// The mipped swapchains (the UI layer's chains, the VR page): one level when VR_UI_NO_MIPS is set
// (`no_mips_env` non-null, any value: every runtime, as before). Else their mip chains on Meta's
// runtime (as before: the one runtime known to handle them) or with VR_SWAPCHAIN_MIPS on
// (`force_env`, EnvOn: to test another runtime), and one level on every other runtime (SteamVR,
// Virtual Desktop, WMR, Pimax...).
constexpr bool SwapchainMips(Runtime runtime, const char* no_mips_env, const char* force_env) {
    if (no_mips_env != nullptr) {
        return false;
    }
    if (EnvOn(force_env)) {
        return true;
    }
    return runtime == Runtime::Meta;
}

// A queue waiting for another queue's timeline value at `value` (VrXrQueue::GameWaitsForRuntime,
// PlanGameBatch): nothing when this queue already waits for it (`waited`: the highest value it
// waits for) or the value is 0 (none); no wait when the host already saw the semaphore reach it
// (`host_seen`: the GPU is past it, so later submissions start after it); else a wait (an empty
// submit, or one in the writer's own batch: the late warp's).
enum class Wait : u8 { None, Host, Queue };

constexpr Wait ChooseWait(u64 waited, u64 value, u64 host_seen) {
    if (value == 0 || value <= waited) {
        return Wait::None;
    }
    if (host_seen >= value) {
        return Wait::Host;
    }
    return Wait::Queue;
}

// The highest value of each timeline semaphore (`key`: its handle) one queue already waits for
// (VrXrQueue::RuntimeWaitsFor): Need() is true when a wait for `value` must still be submitted,
// and records it. A full table forgets its oldest key (one wait too many, never one too few).
template <u32 N>
struct WaitedValues {
    std::array<u64, N> keys{};
    std::array<u64, N> values{};
    u32 next{0};

    constexpr bool Need(u64 key, u64 value) {
        if (key == 0 || value == 0) {
            return false;
        }
        for (u32 i = 0; i < N; ++i) {
            if (keys[i] == key) {
                if (values[i] >= value) {
                    return false;
                }
                values[i] = value;
                return true;
            }
        }
        keys[next] = key;
        values[next] = value;
        next = (next + 1) % N;
        return true;
    }
};

// Several images acquired in one go, one signal for all of them (VrXrQueue::AcquireBatch): Add()
// keeps where each image's value goes, Flush() calls `signal` once (the XR queue's timeline after
// every acquire so far) and writes its value to all of them. A full batch is flushed before it
// takes another (one signal too many, never an image without one).
template <u32 N>
struct SignalBatch {
    std::array<u64*, N> targets{};
    u32 count{0};

    template <typename Signal>
    constexpr void Add(u64& target, Signal&& signal) {
        if (count == N) {
            Flush(signal);
        }
        targets[count++] = &target;
    }

    template <typename Signal>
    constexpr void Flush(Signal&& signal) {
        if (count == 0) {
            return;
        }
        const u64 value = signal();
        for (u32 i = 0; i < count; ++i) {
            *targets[i] = value;
        }
        count = 0;
    }
};

} // namespace Vulkan::VrXrQueuePolicy
