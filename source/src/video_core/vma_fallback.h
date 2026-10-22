// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <atomic>
#include <cstdlib>
#include <vk_mem_alloc.h>
#include <string>
#include <fmt/format.h>
#include "common/crash_record.h"
#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore {

// The allocations made WITHIN_BUDGET fail as soon as this process is over the memory budget the
// system grants it: that budget shrinks with the graphics memory other programs use and on a PC
// short of RAM and page file, and loading the world in VR needs the most at once. Each such
// failure used to end the game ("an internal check failed", 2026-10-17, crash record
// buffer.cpp:139 with the test game's memory capped). Now: the same request past the budget (the
// driver pages instead), then in system memory; only if all fail does the caller's check stop.
// `create` makes the object with the VmaAllocationCreateInfo it is given and returns its result.
// Test only: VR_TEST_OVER_BUDGET=N makes every Nth request's first try report "over budget", so the
// fallback runs on real objects in a normal session. Unset (players): never.
inline u32 TestOverBudgetEvery() {
    static const u32 every = [] {
        const char* v = std::getenv("VR_TEST_OVER_BUDGET");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return every;
}

template <class Create>
VkResult CreateWithFallback(const char* what, VmaAllocationCreateInfo info, Create&& create,
                            bool report_failure = true) {
    static std::atomic<u32> requests{0};
    const u32 every = TestOverBudgetEvery();
    VkResult result = every != 0 && (info.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) &&
                              requests.fetch_add(1) % every == 0
                          ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                          : create(info);
    if (result == VK_SUCCESS || !(info.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT)) {
        return result;
    }
    static std::atomic<u32> over_budget{0}, in_host{0};
    info.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    result = create(info);
    if (result == VK_SUCCESS) {
        const u32 n = over_budget.fetch_add(1) + 1;
        if ((n & (n - 1)) == 0) { // 1, 2, 4, 8, ...
            LOG_WARNING(Render_Vulkan, "VR memory: {} past the memory budget ({} so far)", what, n);
        }
        return result;
    }
    if (info.usage != VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
        info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        result = create(info);
        if (result == VK_SUCCESS) {
            const u32 n = in_host.fetch_add(1) + 1;
            if ((n & (n - 1)) == 0) {
                LOG_WARNING(Render_Vulkan, "VR memory: {} in system memory, the graphics memory "
                                           "is full ({} so far)", what, n);
            }
        }
    }
    if (result != VK_SUCCESS && report_failure) {
        // Nothing left to try: the caller's check ends the game. Say why, for the launcher.
        const std::string note =
            fmt::format("out of memory: {} could not be allocated (Vulkan error {})", what,
                        static_cast<int>(result));
        Common::CrashRecord::Note(note.c_str());
    }
    return result;
}

} // namespace VideoCore
