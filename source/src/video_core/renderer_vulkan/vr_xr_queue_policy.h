// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
#include <limits>
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
    // MockXR (the test runtime) stands in for a Quest 3 on Link: classed as
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

// A +2/+0 prediction sequence may leave a usable slot between the predictions. Target it on
// the +2 so the +0 can use its original slot. A duplicate (+0) of the refresh the previous
// frame already claimed moves one refresh later, so the two do not share it (SteamVR would show
// only the later one). Over Link SteamVR predicts 53-63 ms ahead, so the duplicate's refresh is
// always well in the future here; requiring it to have started (2e38f7ddd) left every unpaired
// +0 sharing its slot. Normal predictions never inherit a one-refresh delay, and a duplicate
// moves at most one refresh past its prediction.
struct SteamVrTargetHistory {
    s64 prediction{};
    s64 target{};
    s64 period{};

    constexpr s64 Next(s64 predicted, s64 current_period, s64 now) const {
        if (target <= 0 || prediction <= 0 || period <= 0 || predicted <= 0 ||
            current_period < 2 || now <= 0) {
            return predicted;
        }
        const s64 period_change = current_period - period;
        if (period_change > period / 100 || period_change < -period / 100) {
            return predicted; // A new refresh rate needs a fresh target history.
        }
        const s64 half_period = current_period / 2;
        const s64 prediction_step = predicted - prediction;
        // OpenXR's target is the middle of the refresh. Leave 2 ms around its start for clock
        // conversion and submit overhead. The caller only supplies a calibrated XR clock.
        constexpr s64 margin = 2'000'000;
        const s64 lead = half_period + margin;
        // Real predictions drift by microseconds: an ordinary one right after a moved duplicate
        // can land just before that duplicate's target, in the same refresh. Ask for that same
        // time rather than one stepping back; a whole refresh or more back is a new timeline.
        const auto in_order = [this, half_period](s64 result) {
            return result < target && target - result < half_period ? target : result;
        };
        if (predicted > current_period && prediction_step >= current_period &&
            prediction_step - current_period >= half_period) {
            const s64 earlier = predicted - current_period;
            if (earlier - target >= half_period && earlier - now >= lead) {
                return earlier;
            }
        }
        if (prediction_step <= -half_period || prediction_step >= half_period ||
            predicted - target >= half_period ||
            predicted > std::numeric_limits<s64>::max() - current_period) {
            return in_order(predicted);
        }
        const s64 later = predicted + current_period;
        if (later - now >= lead) {
            return in_order(later); // (a period a few us shorter must not step back either)
        }
        // Too late even for the next refresh (a stall): the previous request, while still ahead,
        // rather than a prediction already behind it (measured 2026-11-21: T+0.002 ms asked at
        // T+6 ms while T+11.001 ms was still to come).
        if (target > predicted && target > now) {
            return target;
        }
        return in_order(predicted);
    }
};

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

// SteamVR's system name ends with its headset driver after " : ": "SteamVR/OpenXR : oculus" is a
// Quest through Meta's Link or Air Link (SteamVR stacked on Meta's compositor, 2026-11-21 logs);
// native SteamVR headsets name their own driver ("lighthouse" for Index and Vive...). The
// display-time retarget answers the double takes of that stack, so it is for it only.
constexpr bool SteamVrOverMetaLink(std::string_view system_name) {
    const std::size_t colon = system_name.rfind(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    std::string_view driver = system_name.substr(colon + 1);
    while (!driver.empty() && (driver.front() == ' ' || driver.front() == '\t')) {
        driver.remove_prefix(1);
    }
    while (!driver.empty() && (driver.back() == ' ' || driver.back() == '\t')) {
        driver.remove_suffix(1);
    }
    constexpr std::string_view oculus = "oculus";
    if (driver.size() != oculus.size()) {
        return false;
    }
    for (std::size_t i = 0; i < oculus.size(); ++i) {
        const char c = driver[i] >= 'A' && driver[i] <= 'Z' ? static_cast<char>(driver[i] + 32)
                                                            : driver[i];
        if (c != oculus[i]) {
            return false;
        }
    }
    return true;
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
