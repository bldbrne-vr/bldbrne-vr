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
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "core/vr/seqlock.h"

namespace VR::Quick {

struct UseAudioFrame {
    std::int64_t ns{};
    float progress{-1.0f}; // < 0: silent; 0..1: building toward use
    float volume{1.0f};
};

// Presenter is the sole writer; the main audio port is the sole playback owner.
inline SeqLock<UseAudioFrame>& UseAudioChannel() {
    static SeqLock<UseAudioFrame> channel;
    return channel;
}
inline void PublishUseAudio(float progress, float volume, std::int64_t now) {
    UseAudioChannel().Store({now, progress, volume});
}
inline UseAudioFrame LatestUseAudio() {
    UseAudioFrame frame{};
    UseAudioChannel().Load(frame, 4);
    return frame;
}

// A soft band-limited air sound, generated without assets, allocations or locks in playback.
// Pressure/hold progress raises both loudness and brightness. Smooth attack and a 15 ms release
// leave the game's completion sound clear; stale input cannot leave a whoosh running in menus.
// Mixed before backend volume/mute, on front L/R for both SDL and OpenAL (including 7.1).
class UseAudioMixer {
public:
    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             UseAudioFrame state, std::int64_t now) {
        if (!buffer || !frames || rate < 8000 ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const bool fresh = state.ns > 0 && now >= state.ns && now - state.ns < 150'000'000;
        const bool active = fresh && std::isfinite(state.progress) && state.progress >= 0 &&
                            std::isfinite(state.volume) && state.volume > 0;
        const float progress = active ? std::clamp(state.progress, 0.0f, 1.0f) : progress_;
        const float target = active ? (0.08f + 0.18f * progress * progress) *
                                          std::clamp(state.volume, 0.0f, 2.0f)
                                    : 0.0f;
        if (target == 0 && gain_ == 0) return;
        const float smoothing = 1.0f - std::exp(-1.0f / (0.015f * rate));
        const float release = 0.52f / (0.015f * rate); // even maximum volume stops within 15 ms
        const float highpass = 1.0f - std::exp(-6.2831853f * 150.0f / rate);
        // Smoothed coefficients keep pressure changes from introducing clicks.
        const float cutoff = std::min(450.0f + 2750.0f * progress, 0.4f * rate);
        const float lowpass = 1.0f - std::exp(-6.2831853f * cutoff / rate);
        progress_ = progress;
        if (is_float) {
            MixSamples(static_cast<float*>(buffer), frames, channels, target, lowpass,
                       highpass, smoothing, release);
        } else {
            MixSamples(static_cast<std::int16_t*>(buffer), frames, channels, target, lowpass,
                       highpass, smoothing, release);
        }
    }

private:
    template <class T> static void Add(T& destination, float voice) {
        if (voice == 0) return;
        if constexpr (std::is_same_v<T, float>) {
            destination = std::clamp(destination + voice, -1.0f, 1.0f);
        } else {
            destination = static_cast<T>(std::clamp(
                std::lround(destination + voice * 32768.0f), -32768L, 32767L));
        }
    }
    template <class T>
    void MixSamples(T* output, std::size_t frames, unsigned channels, float target,
                    float lowpass, float highpass, float smoothing, float release) {
        for (std::size_t i = 0; i < frames; ++i) {
            gain_ = target > 0 ? gain_ + (target - gain_) * smoothing
                              : std::max(0.0f, gain_ - release);
            if (gain_ == 0) {
                low_ = bass_ = 0;
                break;
            }
            // Deterministic, independent of the game's random numbers.
            noise_ ^= noise_ << 13;
            noise_ ^= noise_ >> 17;
            noise_ ^= noise_ << 5;
            const float noise = static_cast<float>(noise_ >> 8) / 8388608.0f - 1.0f;
            lowpass_ += (lowpass - lowpass_) * smoothing;
            low_ += lowpass_ * (noise - low_);
            bass_ += highpass * (low_ - bass_);
            const float voice = (low_ - bass_) * gain_;
            // A gentle left bias matches the item hand; never put this cue in LFE/rear channels.
            Add(output[i * channels], voice * (channels == 1 ? 1.0f : 0.8f));
            if (channels > 1) Add(output[i * channels + 1], voice * 0.6f);
        }
    }
    std::uint32_t noise_{0x9e3779b9};
    float gain_{}, progress_{}, low_{}, bass_{}, lowpass_{};
};
} // namespace VR::Quick
