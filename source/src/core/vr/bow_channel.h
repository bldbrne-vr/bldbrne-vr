// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

#include "core/vr/seqlock.h"

// Simon's Bowblade's bow (vr.ini bow; core/vr/bow.h): the presenter's newest shot for the gun's
// emitter hook, which turns the game's arrow onto its line (bloodborne_gun.cpp, the bow branch).
// Header-only (one SeqLock, one writer: the XR frame thread).
namespace VR::Combat {

struct BowShot {
    std::uint32_t serial{0}; // +1 per shot
    bool charged{false};     // the charged shot (R2 let go), else the quick one (R1)
    float draw{0.0f};        // 0 .. 1
    float dir[3]{0.0f, 0.0f, -1.0f}; // the arrow's line (OpenXR LOCAL, unit)
    std::int8_t bow_hand{0};         // 0 left, 1 right
    std::uint32_t space_serial{0};   // the tracking space it is in (recenters)
    std::int64_t release_ns{0};      // steady clock
};

inline SeqLock<BowShot>& BowShotChannel() {
    static SeqLock<BowShot> channel;
    return channel;
}

// Gameplay owns the native held arrow while this publication is fresh. The phase is
// explicit: hiding a part destroys it, so its absence must never imply an empty hand.
struct BowVisual {
    std::int64_t ns{0};
    bool arrow{false};
};
inline SeqLock<BowVisual>& BowVisualChannel() {
    static SeqLock<BowVisual> channel;
    return channel;
}

// Fast pointer comparison at the shared native weapon-pose hook. The full request
// is checked again by MeleeBowPartPose before any write.
inline std::atomic<std::uintptr_t>& BowPartNode() {
    static std::atomic<std::uintptr_t> node{0};
    return node;
}

} // namespace VR::Combat
