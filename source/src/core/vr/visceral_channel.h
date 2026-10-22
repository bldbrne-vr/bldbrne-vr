// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>

#include "core/vr/seqlock.h"

// Physical viscerals (visceral_thrust=1; core/vr/visceral_thrust.h): the presenter's thrusts for
// the game's update, and the game's "a visceral is open" for the presenter's heartbeat cue.
// Header-only (one SeqLock each, one writer each).
namespace VR::Combat {

// Presenter -> game (the XR frame thread writes it every physical-melee frame).
struct ThrustState {
    std::uint32_t thrust_serial{0};  // thrusts so far
    std::uint32_t thrust_window{0};  // the melee window open at the newest thrust (0: none)
    std::int64_t thrust_ns{0};       // the newest thrust (steady clock)
    std::uint32_t verdict_window{0}; // the newest window judged
    std::uint8_t verdict{0};         // its VR::Melee::Thrust::Verdict
    const char* why{""};             // ... and what decided it (a string literal)
    bool enabled{false};             // visceral_thrust on, physical melee, swings judged
    float peak{0.0f};                // the newest thrust: m/s along the blade (logs)
    float travel{0.0f};              // ... m along the blade (logs)
    std::int64_t ns{0};              // published at (steady clock)
};

// Game -> presenter (the main player's update writes it).
struct VisceralOpen {
    std::uint32_t serial{0}; // viscerals opened so far (a target the hunter hit staggered)
    bool open{false};        // one is open now
    float distance{0.0f};    // m to that target
    const void* target{nullptr}; // the staggered character while open (the game's; only compared)
    std::int64_t ns{0};      // written at (steady clock)
};

// The interactive visceral (visceral_pull; core/vr/visceral_pull.h).
// Game -> presenter (the main player's frame-time hook writes it every update).
struct VisceralHold {
    std::uint32_t serial{0};        // throws so far (available before the catch)
    bool active{false};             // the hunter is in this throw, including entry and extraction
    bool failed{false};             // mistimed pull: reduced damage, the original pair still plays
    bool released{false};           // no more pull input is needed
    bool held{false};               // the throw waits for the pull now (the catch)
    bool camera_valid{false};       // separate native/finishing-animation camera lifetime is known
    bool camera_active{false};      // unaffected by pull release or a victim leaving its throw
    bool camera_cancelled{false};   // interrupted; latched until the native throw state clears
    std::int64_t held_ns{0};        // the newest hold began (steady clock)
    const char* why{""};            // how the newest hold ended (a string literal)
    std::int64_t ns{0};             // written at (steady clock)
};

// Presenter -> game (the XR frame thread, every frame of a catch until the yank): the hand's pull,
// measured from where it was at the catch.
struct VisceralPulled {
    std::uint32_t serial{0}; // the throw it is for (0: none yet)
    float progress{0.0f};    // 0 .. 1: the hand out of its deepest point (1: visceral_pull_cm)
    bool pulled{false};      // the yank that completes it (the last write for that throw)
    float travel{0.0f};      // m (logs)
    float peak{0.0f};        // m/s (logs)
    std::int64_t ns{0};      // the yank (steady clock; 0: not yet)
};

// Damage hook -> presenter: actual rip, including the first learning throw (the burst in the hand).
struct VisceralRip {
    std::uint32_t serial{0}; // the throw's serial, once per throw
    std::int64_t ns{0};
    bool mistimed{false};
};

inline SeqLock<VisceralRip>& VisceralRipChannel() {
    static SeqLock<VisceralRip> channel;
    return channel;
}

inline SeqLock<VisceralHold>& VisceralHoldChannel() {
    static SeqLock<VisceralHold> channel;
    return channel;
}

inline SeqLock<VisceralPulled>& VisceralPulledChannel() {
    static SeqLock<VisceralPulled> channel;
    return channel;
}

inline SeqLock<ThrustState>& ThrustChannel() {
    static SeqLock<ThrustState> channel;
    return channel;
}

inline SeqLock<VisceralOpen>& VisceralOpenChannel() {
    static SeqLock<VisceralOpen> channel;
    return channel;
}

} // namespace VR::Combat
