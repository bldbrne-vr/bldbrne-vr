// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "core/vr/furnace_audio.h"
#include "core/vr/seqlock.h"

namespace VR::FurnaceAudio {
namespace {
std::array<SeqLock<ClipAudio::Frame>, ClipCount> clips;
SeqLock<Grind::GrindFrame> growl;
} // namespace

void Play(Clip clip, std::int64_t now, float volume, float pitch) {
    if (clip >= ClipCount) {
        return;
    }
    static std::array<ClipAudio::Frame, ClipCount> frames{}; // the grip tick is the only writer
    auto& frame = frames[clip];
    ++frame.id;
    frame.ns = now;
    frame.state = ClipAudio::PlayOut;
    frame.volume = volume;
    frame.pitch = pitch;
    clips[clip].Store(frame);
}

float PullPitch(float base) {
    static std::uint32_t seed = 0x6C8E9CF5u; // (the grip tick only)
    seed = seed * 1664525u + 1013904223u;
    const float u = static_cast<float>(seed >> 8) / 16777216.0f; // 0..1
    return base * (1.0f + PitchJitter * (2.0f * u - 1.0f));
}

ClipAudio::Frame Latest(Clip clip) {
    // A bounded seqlock read can overlap a publication: keep this thread's last good frame (a
    // reset id would replay the last start).
    thread_local std::array<ClipAudio::Frame, ClipCount> frames{};
    if (clip >= ClipCount) {
        return {};
    }
    clips[clip].Load(frames[clip], 4);
    return frames[clip];
}

void PublishGrowl(const Grind::GrindFrame& frame) {
    growl.Store(frame);
}

Grind::GrindFrame LatestGrowl() {
    Grind::GrindFrame frame{};
    growl.Load(frame, 4);
    return frame;
}

} // namespace VR::FurnaceAudio
