// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <fmt/format.h>
#include "common/types.h"

// The frame timeline (logs-on builds, 2026-11-05): one flip's timestamps from the game's submit to
// its publication to the headset, so a slow frame's report (Common::Slow::FrameBoundary) says
// where its time went:
//   game gap   - since the game's previous flip submit (the game's own frame time)
//   to stage A - submit -> the GPU command thread reaches the flip (it was decoding/waiting before)
//   gpu finish - the frame boundary's wait for this frame's rendering (VR::OnGpuFrameBoundary)
//   frame wait - waiting for a free output frame (Presenter::GetRenderFrame)
//   to publish - the frame's copy recorded and submitted, published to the headset
// Stage A handles one flip at a time, in order: its marks are its current flip's.
namespace Common::FrameTimeline {

#ifdef BBVR_NO_LOGS
inline s64 Now() noexcept {
    return 0;
}
inline void Decoded(s64) noexcept {}
inline void Rendered() noexcept {}
inline void FrameWaitStart() noexcept {}
inline void FrameWaitEnd() noexcept {}
#else
inline s64 Now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Marks {
    std::atomic<s64> submitted{}, prev_submitted{}, decoded{}, rendered{}, wait_start{},
        wait_end{};
};
inline Marks g_marks;

/// The GPU command thread reaches the flip the game submitted at `submitted`.
inline void Decoded(s64 submitted) noexcept {
    g_marks.prev_submitted.store(g_marks.submitted.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    g_marks.submitted.store(submitted, std::memory_order_relaxed);
    g_marks.decoded.store(Now(), std::memory_order_relaxed);
    g_marks.rendered.store(0, std::memory_order_relaxed);
    g_marks.wait_start.store(0, std::memory_order_relaxed);
    g_marks.wait_end.store(0, std::memory_order_relaxed);
}
inline void Rendered() noexcept {
    g_marks.rendered.store(Now(), std::memory_order_relaxed);
}
inline void FrameWaitStart() noexcept {
    g_marks.wait_start.store(Now(), std::memory_order_relaxed);
}
inline void FrameWaitEnd() noexcept {
    g_marks.wait_end.store(Now(), std::memory_order_relaxed);
}

/// At the publication: the current flip's parts, " || timeline: ..." (empty without a flip).
inline std::string Report() {
    const s64 now = Now();
    const s64 submitted = g_marks.submitted.load(std::memory_order_relaxed);
    const s64 prev = g_marks.prev_submitted.load(std::memory_order_relaxed);
    const s64 decoded = g_marks.decoded.load(std::memory_order_relaxed);
    const s64 rendered = g_marks.rendered.load(std::memory_order_relaxed);
    const s64 wait_start = g_marks.wait_start.load(std::memory_order_relaxed);
    const s64 wait_end = g_marks.wait_end.load(std::memory_order_relaxed);
    if (submitted == 0 || decoded < submitted) {
        return {};
    }
    const auto ms = [](s64 a, s64 b) { return a != 0 && b >= a ? (b - a) * 1e-6 : -1.0; };
    return fmt::format(" || timeline: game gap {:.1f} | to stage A {:.1f} | gpu finish {:.1f} | "
                       "frame wait {:.1f} | to publish {:.1f} ms",
                       prev != 0 ? ms(prev, submitted) : -1.0, ms(submitted, decoded),
                       ms(decoded, rendered), ms(wait_start, wait_end),
                       ms(wait_end != 0 ? wait_end : decoded, now));
}
#endif

} // namespace Common::FrameTimeline
