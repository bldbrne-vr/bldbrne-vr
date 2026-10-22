// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <atomic>
#include <fmt/format.h>
#include <string_view>

namespace Common::RenderDiagnostic {
// Called on the render thread only. Observe the first immersive entry, then
// sample five immersive flips within a 120-immersive-flip window. Never rearm on menu re-entry.
struct FirstStereoTransition {
    unsigned flips = 0;
    bool saw_flat = false;
    unsigned Observe(bool immersive) noexcept {
        if (flips >= 120) return 0;
        if (!immersive) { if (!flips) saw_flat = true; return 0; }
        ++flips;
        return flips == 1 || flips == 2 || flips == 8 || flips == 30 || flips == 120
            ? flips : 0;
    }
};
// Independent of NO_LOGS. Bounded local records, no pixel readbacks or telemetry.
void Write(std::string_view message) noexcept;
}

// Evaluate/format arguments only on the first occurrence at this call site.
#define RENDER_DIAGNOSTIC_ONCE(...) do { \
    static std::atomic<bool> recorded{false}; \
    if (!recorded.load(std::memory_order_relaxed) && \
        !recorded.exchange(true, std::memory_order_relaxed)) { \
        try { Common::RenderDiagnostic::Write(fmt::format(__VA_ARGS__)); } catch (...) {} \
    } \
} while (false)
