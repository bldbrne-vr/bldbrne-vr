// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include "core/vr/clip_audio.h"
#include "core/vr/item_audio.h"

// Tonitrus charged (core/vr/tonitrus_charge.h): a thunderclap with the bolt from the sky: the
// Tiny Tonitrus' own lightning (2026-11-09: "maybe we use the tiny tonitrus thunder
// effect"; its chain's FFX 203007 plays event s000203007, one of three takes). Decoded once at
// startup from the player's bank (core/vr/game_sounds.cpp; nothing of the game is shipped), the
// layers mixed into one clip. Started by the presenter at the charge's start, played to its end by
// the shared clip player (core/vr/clip_audio.h), centred into the main port.
namespace VR::ThunderAudio {

struct Layer {
    ItemAudio::CutSpec cut;
    std::size_t at_ms; // after the clap's start
    float gain;        // of the cut brought to full scale
};
// Take A, the user's pick of the audition ("6_tiny_tonitrus_A ... as the hunter strike on our
// tonitrus weapon"): loud from its first moment, a crackling roll swelling to 0.9 s, gone by 1.7 s.
inline constexpr std::array<Layer, 1> Layers{{
    {{"s000203007", 0, 2050, 4, 300}, 0, 0.9f},
}};
inline constexpr float Gain = 0.6f;

// The layers mixed into one 24 kHz mono S16LE clip: each cut brought to full scale, then its gain,
// at its offset; the sum held under 0.89 of full scale. A cut that did not decode is left out
// (empty: nothing decoded).
inline std::vector<char> Mix(std::span<const std::vector<char>> cuts,
                             std::span<const Layer> layers) {
    std::size_t total = 0;
    for (std::size_t k = 0; k < cuts.size() && k < layers.size(); ++k) {
        if (!cuts[k].empty()) {
            total = std::max(total, layers[k].at_ms * 24 + cuts[k].size() / 2);
        }
    }
    if (total == 0) {
        return {};
    }
    const auto sample = [](const std::vector<char>& pcm, std::size_t i) {
        const auto lo = static_cast<unsigned char>(pcm[i * 2]);
        const auto hi = static_cast<unsigned char>(pcm[i * 2 + 1]);
        return static_cast<float>(static_cast<std::int16_t>(lo | (hi << 8))) / 32768.0f;
    };
    std::vector<float> sum(total, 0.0f);
    for (std::size_t k = 0; k < cuts.size() && k < layers.size(); ++k) {
        const auto& pcm = cuts[k];
        const std::size_t n = pcm.size() / 2;
        float peak = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            peak = std::max(peak, std::abs(sample(pcm, i)));
        }
        if (peak <= 0.0f) {
            continue;
        }
        const float g = layers[k].gain / peak;
        const std::size_t at = layers[k].at_ms * 24;
        for (std::size_t i = 0; i < n; ++i) {
            sum[at + i] += sample(pcm, i) * g;
        }
    }
    float peak = 0.0f;
    for (const float v : sum) {
        peak = std::max(peak, std::abs(v));
    }
    const float scale = peak > 0.89f ? 0.89f / peak : 1.0f;
    std::vector<char> out(total * 2);
    for (std::size_t i = 0; i < total; ++i) {
        const auto v = static_cast<std::int16_t>(
            std::lround(std::clamp(sum[i] * scale, -1.0f, 1.0f) * 32767.0f));
        out[i * 2] = static_cast<char>(v & 0xFF);
        out[i * 2 + 1] = static_cast<char>((v >> 8) & 0xFF);
    }
    return out;
}

// The presenter's frame thread only: one more start (PlayOut: it plays to its end).
void Play(std::int64_t now);
ClipAudio::Frame Latest();

class Mixer : public ClipAudio::Player {
public:
    Mixer() : ClipAudio::Player({Gain}) {}
};

} // namespace VR::ThunderAudio
