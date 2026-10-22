// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Shorter hit staggers (vr.ini stagger_shorten_ms, 150): the hunter's hit staggers (a000_020000 ..
// 020399, the on-feet reactions the arms keep; never a knockdown, launch, grab, guard reaction or
// death) play faster from their start, at up to MaxRate times the game's speed, until the clip has
// gained shorten_ms on the clock; then at the game's speed again. Everything the clip's TAE opens
// later (R1 / R2, L2, the move and dodge cancels, items, the gun) and its end come shorten_ms
// sooner, all at once, because the game itself runs the clip faster: the hunter's frame time
// (ChrIns+0xE0, the time his update, his behaviour graph, the TAE and the root motion run on) is
// scaled for those updates, so the state machine never sees a window the clip has not reached.
// Our own followers (the stagger vignette's clear, the swing buffer, the gun and item waits) read
// the game's windows, so they move with it.
//
// A window already open (the game takes an attack or a move again) ends the speed-up at once (the
// clip's own time is then the game's again), and so does a new clip; a new blow mid-stagger (the
// same id from its start again, or another stagger id) starts over. The light staggers whose R1
// window opens 0.32 s into the clip reach it after 0.18 s (133 ms sooner) at MaxRate 1.75; the
// rest gain the full 150 ms within their first 0.2 s (733 -> 583 ms in the arena).
//
// Pure (no emulator state): core/vr/bloodborne_stagger.cpp reads the game's TimeAct ring and the
// action request module at the hunter's update; tests/test_stagger.cpp drives it.
namespace VR::StaggerSpeed {

constexpr float MaxRate = 1.75f;
// The request mask of the update before the stagger's first TAE step still holds the last clip's
// windows (an idle hunter takes R1): a window counts this far into the clip at the earliest (as
// anim_classes.h CancelMinNs; the earliest real ones open at 0.23 s).
constexpr float WindowMinS = 0.1f;
constexpr float MaxDt = 0.1f;

constexpr bool IsHitStagger(std::int32_t id) {
    const std::int32_t base = id % 1'000'000;
    return id >= 0 && base >= 20000 && base <= 20399;
}

struct In {
    std::int32_t anim_id{-1}; // the TimeAct ring's newest id (-1: none)
    float clip_time{0.0f};    // its time after the last step (the entry's second float), s
    bool window{false};       // the game takes R1 / R2 / L2 or a move cancels the clip
    float dt{0.0f};           // this update's frame time, s (the game's)
    float shorten_s{0.0f};    // stagger_shorten_ms / 1000
};

class Shortener {
public:
    // The factor for this update's frame time (1 = the game's).
    float Step(const In& in) {
        const bool stagger = IsHitStagger(in.anim_id);
        const bool fresh = in.anim_id != id_ || in.clip_time + 0.02f < time_;
        id_ = in.anim_id;
        time_ = in.clip_time;
        if (fresh) {
            if (active_) {
                ended_ = End::NewClip;
            }
            gained_ = 0.0f;
            active_ = stagger && in.shorten_s > 0.0f;
            started_ = active_;
        }
        if (!active_) {
            return 1.0f;
        }
        if (!stagger) {
            active_ = false;
            ended_ = End::NewClip;
            return 1.0f;
        }
        if (in.window && in.clip_time >= WindowMinS) {
            active_ = false;
            ended_ = End::Window;
            return 1.0f;
        }
        const float dt = std::clamp(in.dt, 0.0f, MaxDt);
        const float left = in.shorten_s - gained_;
        if (!(dt > 0.0f) || !(left > 0.0f)) {
            if (!(left > 0.0f)) {
                active_ = false;
                ended_ = End::Gained;
            }
            return 1.0f;
        }
        const float rate = std::min(MaxRate, 1.0f + left / dt);
        gained_ += dt * (rate - 1.0f);
        if (gained_ >= in.shorten_s - 1e-6f) {
            active_ = false;
            ended_ = End::Gained;
        }
        return rate;
    }

    enum class End : std::uint8_t { None, Gained, Window, NewClip };
    // How the last speed-up ended (taken: None afterwards), for the log.
    End TakeEnded() {
        const End e = ended_;
        ended_ = End::None;
        return e;
    }
    // A speed-up began with the last Step (taken).
    bool TakeStarted() {
        const bool s = started_;
        started_ = false;
        return s;
    }
    bool Active() const {
        return active_;
    }
    float Gained() const {
        return gained_;
    }
    std::int32_t Id() const {
        return id_;
    }

private:
    std::int32_t id_{-1};
    float time_{0.0f};
    float gained_{0.0f};
    bool active_{false};
    bool started_{false};
    End ended_{End::None};
};

} // namespace VR::StaggerSpeed
