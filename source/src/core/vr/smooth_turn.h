// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include "common/types.h"

// Smooth turning by time (VR::SetSmoothTurnRate). The presenter reads the stick every XR frame and
// publishes the turn's speed; each camera update turns by that speed times its own time since the
// last one. Queued per XR frame instead (VR::AddTurn, before 2026-11-06), a game update landing
// right next to an XR frame took two frames' turn and the next one none: at 90 / 90 Hz 0 and 2.7
// degrees instead of 1.3 each, a judder through the whole turn that a load could lock in (the
// two clocks' phase) until the next load or a restart. Snap turns and the quick turn stay queued
// (VR::AddTurn): one step each. Pure helpers here (unit-tested).
namespace VR::SmoothTurn {

// A speed older than this is not taken (the stick is not being read: a menu, the VR page, the
// presenter stopped): no turn.
constexpr s64 StaleNs = 50'000'000;
// At most this much time in one update (a hitch, a gap): the turn it would have queued.
constexpr s64 MaxStepNs = 100'000'000;

// The turn (radians, + = right) for an update at `now` whose time runs from `from_ns` (the last
// update's; 0 = none yet: nothing), at the speed `rate` (radians a second) published at `rate_ns`.
[[nodiscard]] inline float Step(float rate, s64 rate_ns, s64 from_ns, s64 now) {
    if (from_ns <= 0 || now <= from_ns || rate == 0.0f || now - rate_ns > StaleNs) {
        return 0.0f;
    }
    return rate * static_cast<float>(std::min(now - from_ns, MaxStepNs)) * 1e-9f;
}

} // namespace VR::SmoothTurn
