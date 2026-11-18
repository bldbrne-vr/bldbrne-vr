// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include "common/types.h"

namespace Vulkan::Residency {

// Where Windows keeps this process's GPU memory now: Task Manager's "GPU Process Memory" counters
// (Local / Non Local Usage). Neither VK_EXT_memory_budget nor DXGI's QueryVideoMemoryInfo can
// tell: both count an allocation where it was made, also after Windows moved it to system memory
// (2026-11-05 hog test: both said 0 moved while the counters showed 0.4-0.9 GB). Windows only.
class Reader {
public:
    static constexpr int PeriodMs = 2000;

    // The adapter with this LUID (VkPhysicalDeviceVulkan11Properties::deviceLUID).
    explicit Reader(const std::array<u8, 8>& luid);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    struct Usage {
        u64 local{};     ///< resident in the GPU's memory
        u64 non_local{}; ///< in system memory (staging and readback buffers, and what Windows moved)
        u64 sample{};    ///< which collection (1, 2, ...): a reader acts once per new one
    };
    // The last reading; nullopt before the first, where none can be had, or when the last good
    // one is older than StaleMs (the counters failed since: no decision on old numbers).
    static constexpr int StaleMs = 3 * PeriodMs;
    [[nodiscard]] std::optional<Usage> Read() const;
    [[nodiscard]] bool Found() const noexcept {
        return found;
    }

private:
    bool found{};
    // The last reading, both counters from one collection, and when it was made (steady clock).
    mutable std::mutex reading_mutex;
    Usage reading{};
    std::chrono::steady_clock::time_point read_at{};
    bool have_reading{};
    std::jthread sampler; // last: stops before the reading above goes
};

} // namespace Vulkan::Residency
