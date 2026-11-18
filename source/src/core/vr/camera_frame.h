// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include "core/vr/vr_session.h"

namespace VR {

// Callers hold record_mutex. An undriven record marks a cutscene or another hand-back.
// Optional lag may reach the first headset pose after that hand-back, but never cross it.
template <std::size_t N>
const CameraRecord& LaggedCameraRecord(const std::array<CameraRecord, N>& records, u64 frame,
                                       u64 latest, s32 lag) {
    const auto& own = records[frame % N];
    if (own.id != frame) {
        return records[latest % N];
    }
    if (lag <= 0 || !own.driven) {
        return own;
    }
    const auto steps = static_cast<u64>(lag);
    if (steps >= N || steps >= frame || records[(frame - steps) % N].id != frame - steps) {
        return own; // outside the recorded history
    }
    u64 id = frame;
    while (id > frame - steps && records[(id - 1) % N].driven) {
        --id;
    }
    return records[id % N];
}

// The render-camera hook can still be filling in this update's projection. Prefer the
// newest ready one in the same uninterrupted sequence of headset-driven records.
template <std::size_t N>
const CameraRecord& DrawProjectionRecord(const std::array<CameraRecord, N>& records,
                                         const CameraRecord& own, u64 latest) {
    if (own.has_projection || !own.driven) {
        return own;
    }
    u64 id = own.id;
    while (id < latest) {
        const auto& next = records[(id + 1) % N];
        if (next.id != id + 1 || !next.driven) {
            break;
        }
        ++id;
    }
    while (id != 0) {
        const auto& candidate = records[id % N];
        if (candidate.id != id || !candidate.driven) {
            break;
        }
        if (candidate.has_projection) {
            return candidate;
        }
        --id;
    }
    return own;
}

} // namespace VR
