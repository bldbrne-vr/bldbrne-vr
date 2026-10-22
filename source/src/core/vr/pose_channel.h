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

// Physical gestures (pose_gestures; core/vr/pose_gestures.h): the game's zone and gates for the
// presenter's recogniser, and the recognised pose for the game's update. Header-only (one SeqLock
// each, one writer each).
namespace VR::PoseGesture {

// Game -> presenter (the main player's update: every update in a zone, once a second elsewhere).
struct ZoneState {
    std::uint8_t zone{0};        // Zone
    bool blocked{false};         // a gate says no (GateBlock): no tick, no marker
    std::uint32_t available{0};  // the poses that count (AvailableKinds)
    std::uint32_t fired{0};      // markers applied so far ...
    std::int32_t fired_marker{-1}; // ... the newest one's first marker (164: the Brain answered)
    std::int64_t ns{0};          // written at (steady clock)
};

// Presenter -> game (the XR frame thread, every gameplay frame while a zone is on).
struct PoseState {
    std::uint8_t pose{0};     // Kind (0: none)
    float held_s{0.0f};       // held this long
    std::uint32_t serial{0};  // the hold's serial (+1 per recognition)
    std::int64_t ns{0};       // written at (steady clock)
};

inline SeqLock<ZoneState>& ZoneChannel() {
    static SeqLock<ZoneState> channel;
    return channel;
}

inline SeqLock<PoseState>& PoseChannel() {
    static SeqLock<PoseState> channel;
    return channel;
}

} // namespace VR::PoseGesture
