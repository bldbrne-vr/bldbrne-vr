// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include "core/vr/vitals.h"

// Immersive vitals across threads (single-writer seqlocks, core/vr/seqlock.h):
//  - the hunter's HP, stamina and bullets, read by site A every main-player update
//    (bloodborne_vitals.cpp VitalsSiteA);
//  - the Blood Gauge's state from the render-camera hook (VitalsRenderTick): whether it found
//    the hunter's left forearm, and what it shows;
//  - the last HP lost (the heartbeat's mute timer).
namespace VR::Vitals {

// Site A only.
void PublishStatus(const Status& status);
// Any thread. False before the first status.
bool LatestStatus(Status& out);

struct GaugeState {
    std::int64_t tick_ns{0};   // the render tick's last run for a driven first-person frame
    std::int64_t anchor_ns{0}; // ... and the last one that found the forearm's dummies
    std::int64_t drawn_ns{0};  // ... and the last one that submitted the gauge (on the frame)
    float radius_m{0.0f};      // the fitted radius (the plate's apex from the forearm's axis)
    std::uint64_t fits{0};     // depth measurements taken
};

// Render-camera hook only.
void PublishGauge(const GaugeState& state);
// Any thread. False before the first one.
bool LatestGauge(GaugeState& out);

// The heartbeat's clock (the presenter's frame thread runs it, the haptic plays on its beats):
// the gauge on the wrist pulses on the same beats.
struct Beat {
    std::int64_t beat_ns{0}; // the last beat (0: not beating)
    float bpm{0.0f};
};
void PublishBeat(const Beat& beat);
bool LatestBeat(Beat& out);

// Whether the wrist mode works now (else the presenter shows the game's HP cluster as in the
// classic mode): the status read and fresh (1 s) and the gauge anchored within 1 s of the render
// tick's last run. `why` names the first thing missing (a static string), nullptr when it works.
bool WristWorks(std::int64_t now_ns, const char** why = nullptr);

} // namespace VR::Vitals
