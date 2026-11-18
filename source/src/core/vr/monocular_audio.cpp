// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>
#include "core/vr/monocular_audio.h"
#include "core/vr/seqlock.h"

namespace VR::Monocular {
namespace {
SeqLock<Whip::RecallAudioFrame> channel;

// A small lens ring's detent: a click of noise (0.4 ms), the barrel's bright ring (3.4 kHz,
// gone in about 1.2 ms) and a duller body (1.3 kHz, 2.5 ms); 12 ms at 24 kHz, peak at 90%. A
// fixed noise seed, so every tick sounds the same.
std::vector<char> MakeTick() {
    constexpr int Rate = 24000;
    constexpr int Count = Rate * 12 / 1000;
    constexpr float TwoPi = 6.2831853f;
    std::vector<float> wave(Count);
    std::uint32_t seed = 0x9e3779b9u;
    float peak = 0.0f;
    for (int i = 0; i < Count; ++i) {
        const float t = static_cast<float>(i) / Rate;
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>(static_cast<std::int32_t>(seed)) / 2147483648.0f;
        const float v = 0.9f * noise * std::exp(-t / 0.0004f) +
                        0.7f * std::sin(TwoPi * 3400.0f * t) * std::exp(-t / 0.0012f) +
                        0.4f * std::sin(TwoPi * 1300.0f * t) * std::exp(-t / 0.0025f);
        wave[i] = v;
        peak = std::max(peak, std::abs(v));
    }
    std::vector<char> pcm(2 * Count);
    for (int i = 0; i < Count; ++i) {
        const auto s = static_cast<std::int16_t>(
            std::lround(wave[i] / std::max(peak, 1e-6f) * 0.9f * 32767.0f));
        pcm[2 * i] = static_cast<char>(s & 0xff);
        pcm[2 * i + 1] = static_cast<char>((s >> 8) & 0xff);
    }
    return pcm;
}
} // namespace

void PublishLensAudio(bool enabled, bool tick, float pan, std::int64_t now) {
    static Whip::RecallAudioFrame frame{}; // the presenter is the only writer
    frame.ns = now;
    frame.enabled = enabled;
    frame.pan = pan;
    if (enabled && tick) {
        ++frame.clink_id;
        frame.clink_ns = now;
    }
    channel.Store(frame);
}

Whip::RecallAudioFrame LatestLensAudio() {
    // As Gun::LatestReloadAudio: a bounded read can overlap publication; this audio thread keeps
    // its last good frame instead of resetting the tick's id and replaying it.
    thread_local Whip::RecallAudioFrame frame{};
    channel.Load(frame, 4);
    return frame;
}

std::span<const char> LensTickPcm() {
    static const std::vector<char> pcm = MakeTick();
    return pcm;
}
} // namespace VR::Monocular
