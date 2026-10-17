// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace Vulkan {
// Incomplete frames in a row that turn the provider off (a menu change turns it back on).
inline constexpr unsigned TemporalFailureFrames = 8;
// A complete successful stereo resolve resets the streak at the call site.
inline bool TemporalFailure(unsigned& consecutive, bool permanent = false) {
    consecutive = std::min(consecutive, TemporalFailureFrames - 1) + 1;
    return permanent || consecutive >= TemporalFailureFrames;
}
// FAST_GPU_S1_ASYM (vk_rasterizer_temporal.cpp; opt-in: a failed probe shows a jittered frame
// unreconstructed): S1's provider after TemporalFailure's streak,
// or a scene without its post-processing trigger, is not off until a menu change: the guest
// renders at the input size whatever the provider does (the boot patch), so an off provider only
// leaves the reduced image. It waits, configured and idle, and retries; a retry that fails again
// within ProbeFailures frames doubles the wait (up to MaxWait frames), a reconstructed frame
// restores FirstWait. A provider failure (Resolve's Failed/Unavailable, Begin's refusals) is not
// retried (Reset).
struct TemporalRetry {
    static constexpr uint32_t FirstWait = 64, MaxWait = 512;
    static constexpr unsigned ProbeFailures = 2;
    uint64_t at{}; // the frame that retries (0: none scheduled)
    uint32_t wait{FirstWait};
    bool probing{}; // a retry started and has neither reconstructed nor latched again
    bool Scheduled() const { return at != 0; }
    bool Waiting(uint64_t frame) const { return at != 0 && frame < at; }
    // The provider latched off at `frame`.
    void Latch(uint64_t frame) {
        if (probing)
            wait = std::min(wait * 2, MaxWait);
        probing = false;
        at = frame + wait;
    }
    // The retry's first frame: ProbeFailures more incomplete frames in a row latch it again.
    void Start(unsigned& consecutive) {
        at = 0;
        probing = true;
        consecutive = TemporalFailureFrames - ProbeFailures;
    }
    // A reconstructed frame. True when it ends a retry.
    bool Success() {
        const bool ended = probing;
        probing = false;
        wait = FirstWait;
        return ended;
    }
    void Reset() { *this = {}; }
};
}
