// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cstdint>
namespace Vulkan {
// Incomplete scene frames in a row that latch the provider; S1 then schedules recovery.
inline constexpr unsigned TemporalFailureFrames = 8;
// A loading/menu frame with no main scene is not a failed reconstruction. Leave the provider
// ready for the next scene instead of latching or backing off through normal world transitions.
// Hard failures still latch immediately, regardless of scene availability.
inline bool TemporalFailure(unsigned& consecutive, bool permanent = false, bool has_scene = true) {
    if (!permanent && !has_scene) {
        consecutive = 0;
        return false;
    }
    consecutive = std::min(consecutive, TemporalFailureFrames - 1) + 1;
    return permanent || consecutive >= TemporalFailureFrames;
}
// S1 recovery after a soft failure: keep the provider configured and idle until the retry.
// The guest remains at its boot-patched input size, so permanent disable leaves a reduced
// image. The caller renders every probe without jitter and resets scene/provider history;
// only a complete stereo resolve restores normal jitter on the next frame. Repeated failed
// probes back off (at most MaxWait frames). Hard provider failures and user Off reset retries.
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
