// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <type_traits>

// A lever pulled by hand that the game accepted (core/vr/bloodborne_levers.cpp): a heavy latch
// clunk, the game's own o990100001 (chosen in an audition, 2026-10-16), decoded at
// startup from the player's map banks (core/vr/game_sounds.cpp; nothing of the game is shipped).
// A hand pull skips the game's pull animation, whose TAE plays the lever's own sound, so without
// this a pull that worked was silent. Its own one-shot player: it plays to its end (the recall
// mixer's 150 ms freshness would cut a 1.26 s clip short), once per accepted pull, never for a
// pull the game refused or a motion rehearsal.
namespace VR::LeverAudio {

constexpr const char* SampleName = "o990100001";
// The same recording sits in these map banks (identical copies); the first one found is used.
constexpr std::array<const char*, 9> Banks{
    "/app0/dvdroot_ps4/sound/sprj_m24.fsb", "/app0/dvdroot_ps4/sound/sprj_m21.fsb",
    "/app0/dvdroot_ps4/sound/sprj_m22.fsb", "/app0/dvdroot_ps4/sound/sprj_m25.fsb",
    "/app0/dvdroot_ps4/sound/sprj_m27.fsb", "/app0/dvdroot_ps4/sound/sprj_m28.fsb",
    "/app0/dvdroot_ps4/sound/sprj_m29.fsb", "/app0/dvdroot_ps4/sound/sprj_m32.fsb",
    "/app0/dvdroot_ps4/sound/sprj_m35.fsb"};
constexpr float Gain = 0.6f;
// A pull heard by the audio thread later than this (a stall, the audio starting) is not played.
constexpr std::int64_t FreshNs = 300'000'000;

struct Frame {
    std::uint64_t id{0}; // one more per accepted pull
    std::int64_t ns{0};  // when it was accepted (steady clock)
};

// The game thread, at an accepted pull.
void PublishPull(std::int64_t now);
Frame Latest();

// Allocation-free playback of 24 kHz mono S16LE, mixed into the main port before the backend's
// volume, centred.
class Mixer {
public:
    void SetPcm(std::span<const char> pcm) {
        if (!live_ && pcm.size() >= 4) {
            pcm_ = pcm;
        }
    }
    [[nodiscard]] bool HasPcm() const {
        return pcm_.size() >= 4;
    }
    [[nodiscard]] bool Playing() const {
        return live_;
    }

    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             Frame state, std::int64_t now) {
        if (state.id != id_) {
            id_ = state.id;
            if (HasPcm() && state.ns > 0 && now >= state.ns && now - state.ns < FreshNs) {
                pos_ = 0.0;
                live_ = true;
            }
        }
        if (!live_ || !buffer || !frames || !rate ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const double step = 24000.0 / rate;
        if (is_float) {
            MixSamples(static_cast<float*>(buffer), frames, channels, step);
        } else {
            MixSamples(static_cast<std::int16_t*>(buffer), frames, channels, step);
        }
    }

private:
    static float Sample(std::span<const char> pcm, std::size_t index) {
        const auto lo = static_cast<unsigned char>(pcm[index * 2]);
        const auto hi = static_cast<unsigned char>(pcm[index * 2 + 1]);
        const int bits = lo | (hi << 8);
        return static_cast<float>(bits >= 32768 ? bits - 65536 : bits) / 32768.0f;
    }
    template <class T>
    static void Add(T& destination, float voice) {
        if constexpr (std::is_same_v<T, float>) {
            destination = std::clamp(destination + voice, -1.0f, 1.0f);
        } else {
            destination = static_cast<T>(
                std::clamp(std::lround(destination + voice * 32768.0f), -32768L, 32767L));
        }
    }
    template <class T>
    void MixSamples(T* output, std::size_t frames, unsigned channels, double step) {
        const std::size_t count = pcm_.size() / 2;
        // Centred: equal power on the front left and right (common to both console layouts).
        const float side = channels == 1 ? 1.0f : std::sqrt(0.5f);
        for (std::size_t i = 0; i < frames && live_; ++i) {
            const auto a = static_cast<std::size_t>(pos_);
            if (a >= count) {
                live_ = false;
                break;
            }
            const std::size_t b = a + 1 < count ? a + 1 : a;
            const float s = Sample(pcm_, a) +
                            (Sample(pcm_, b) - Sample(pcm_, a)) * static_cast<float>(pos_ - a);
            const float voice = s * Gain * side;
            Add(output[i * channels], voice);
            if (channels > 1) {
                Add(output[i * channels + 1], voice);
            }
            pos_ += step;
        }
    }

    std::span<const char> pcm_{};
    std::uint64_t id_{0};
    double pos_{0.0};
    bool live_{false};
};

} // namespace VR::LeverAudio
