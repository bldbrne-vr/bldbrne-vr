// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <atomic>
#include <fmt/format.h>
#include <string>
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
// Render-thread only. Sample cheaply, then format only the initial value or a change
// observed on two consecutive polls. Flat/menu frames do not reset or consume it.
// Eight records total leave room under Write's process-wide 64-record budget.
template <typename T>
struct SettingsSnapshot {
    bool Poll(bool immersive) noexcept {
        if (!immersive || records >= 8) return false;
        if (countdown) { --countdown; return false; }
        countdown = 119;
        return true;
    }
    bool Observe(const T& value) noexcept {
        if (records >= 8) return false;
        if (!records) {
            last = pending = value;
            ++records;
            return true;
        }
        if (value == last) { pending = value; return false; }
        if (value != pending) { pending = value; return false; }
        last = value;
        ++records;
        return true;
    }
private:
    T last{}, pending{};
    unsigned countdown{}, records{};
};
// Independent of NO_LOGS. Bounded local records, no pixel readbacks or telemetry.
void Write(std::string_view message) noexcept;
// The game being run, as one line (its id, version, add-on, folder): written once to
// render-diagnostics.txt by SetGame, and kept for the crash reports (Game: "Game: unknown" before).
void SetGame(std::string line) noexcept;
std::string Game();
// log/headset.json: the size each eye's image the headset asks for, written at every start (the
// launcher warns when it is too large to render). Replaces the previous launch's file.
void WriteHeadset(std::string_view system, unsigned width0, unsigned height0, unsigned width1,
                  unsigned height1, unsigned max_width, unsigned max_height) noexcept;
}

// Evaluate/format arguments only on the first occurrence at this call site.
#define RENDER_DIAGNOSTIC_ONCE(...) do { \
    static std::atomic<bool> recorded{false}; \
    if (!recorded.load(std::memory_order_relaxed) && \
        !recorded.exchange(true, std::memory_order_relaxed)) { \
        try { Common::RenderDiagnostic::Write(fmt::format(__VA_ARGS__)); } catch (...) {} \
    } \
} while (false)
