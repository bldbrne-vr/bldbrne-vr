// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <cmath>
#include <cstdint>
#include "core/vr/c0000_anim_lengths.h"

namespace VR::GrabCamera {

// Retail c0000 front/rear visceral variants, validated against the TAE lengths.
// Other weapon attacks in the 33xxx range must not start a camera hold by themselves.
inline bool IsVisceralAnimation(std::int32_t clip) {
    return clip >= 100033000 && clip <= 100033210 &&
           Body::C0000::AnimLengthSeconds(clip) > 0;
}

// Camera lifetime is independent of the interactive pull and victim's throw flag.
// A victim can die/leave the pair at the rip while the hunter still bends forward
// in extraction. Only native hunter entry can start this hold; a known finishing
// clip may extend it after native exit. Real interruptions remain authoritative.
class VisceralCameraLifetime {
public:
    bool Step(bool enabled, int state, std::int32_t clip, float clip_time,
              bool cancel_action, const char* interrupt, std::int64_t now) {
        if (last_ns_ && (now < last_ns_ || now - last_ns_ > 5'000'000'000LL)) Reset();
        last_ns_ = now;
        const bool was_native = native_;
        native_ = state > 0 && (state & 1) != 0;
        if (!enabled || (state > 0 && !native_) || interrupt) {
            active_ = false;
            cancelled_ = native_;
            reason_ = !enabled ? "disabled" : interrupt ? interrupt : "incoming grab";
            return false;
        }
        if (native_ && !was_native) cancelled_ = false;
        if (native_) {
            if (active_ && was_native && clip_ >= 0 && cancel_action && clip >= 0 && clip != clip_) {
                cancelled_ = true;
                reason_ = "new player action";
            }
            active_ = !cancelled_;
            if (active_) {
                if (IsVisceralAnimation(clip)) clip_ = clip;
                reason_ = "native hunter throw";
            }
            tail_ns_ = 0;
            return active_;
        }
        cancelled_ = false;
        const float length = Body::C0000::AnimLengthSeconds(clip);
        if (active_ && IsVisceralAnimation(clip) && std::isfinite(clip_time) &&
            clip_time >= 0 && clip_time < length && !cancel_action) {
            if (!tail_ns_) tail_ns_ = now;
            // A stale TimeAct entry must not leave a stationary view forever.
            // The longest known finishing clip is 5.5s; native exit unfreezes it.
            if (now - tail_ns_ <= 7'000'000'000LL) {
                clip_ = clip;
                reason_ = "visceral finishing animation";
                return true;
            }
            reason_ = "finishing animation stopped updating";
        } else {
            reason_ = IsVisceralAnimation(clip) && std::isfinite(clip_time) && clip_time >= length
                          ? "visceral animation completed" : "native throw and finishing clip ended";
        }
        active_ = false;
        return false;
    }
    bool Active() const { return active_; }
    bool Cancelled() const { return cancelled_; }
    const char* Reason() const { return reason_; }
    void Reset() { *this = {}; }
private:
    bool active_{};
    bool native_{};
    bool cancelled_{};
    std::int32_t clip_{-1};
    std::int64_t last_ns_{};
    std::int64_t tail_ns_{};
    const char* reason_{"idle"};
};

} // namespace VR::GrabCamera
