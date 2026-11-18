// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "core/vr/clip_audio.h"

// Two short sounds of an item used at once, played by us where the game's PlaySound could only
// play a whole recording (the tester's 2026-11-04 "a splash sound when the augur is squeezed. A gulp
// when it is swallowed (or other items like antidote pills) ... reuse our existing custom mixers
// ... SUPER PERFORMANT and memory safe", then "too long, we need it much shorter", "just a 'gulp'
// ... short and not too crazy" and, of the audition, "I like the D gulp ... and
// AUDITION_squeeze_short_220ms for squeeze - they shouldn't be too loud"). Both are the game's own
// recordings cut short, decoded once at startup from the player's bank (core/vr/game_sounds.cpp;
// nothing of the game is shipped):
//  - Splash: the squeezed Augur's, blood1 (one of the game's blood splash s820's takes), its first
//    hit;
//  - Gulp: the swallow inside the game's drinking sound (drink_01, c100050), alone: an item eaten
//    at the mouth, the eaten Augur.
// Started by the render tick's use cue (bloodborne_quickitems.cpp PlayUseSounds: a use sound of
// type Quick::OwnSoundType), played to its end by the shared clip player (core/vr/clip_audio.h),
// mixed centred into the main port, quietly. Each is one fixed buffer for the process's life;
// nothing is allocated as it plays and a silent mixer is a few compares.
namespace VR::ItemAudio {

enum Clip : std::uint8_t { Splash = 0, Gulp = 1, ClipCount = 2 };

// Which part of which recording (24 kHz mono S16LE, as decoded): from..to, faded in and out.
struct CutSpec {
    const char* sample;
    std::size_t from_ms, to_ms, fade_in_ms, fade_out_ms;
};
constexpr CutSpec SplashCut{"blood1", 0, 220, 0, 80};
constexpr CutSpec GulpCut{"drink_01", 155, 390, 4, 50};
// Quiet (blood1 peaks at full scale, the swallow at -7 dBFS), before the game's master volume.
constexpr float SplashGain = 0.35f;
constexpr float GulpGain = 0.5f;

// The cut: its samples as decoded, the first fade_in_ms rising from silence (linear) and the last
// fade_out_ms falling to it (cos^2): no click at either end. Ends early with the recording; empty
// if nothing is left between the fades.
inline std::vector<char> Cut(std::span<const char> pcm, const CutSpec& c) {
    const std::size_t total = pcm.size() / 2;
    const std::size_t from = c.from_ms * 24, to = std::min(total, c.to_ms * 24);
    const std::size_t in = c.fade_in_ms * 24, out = c.fade_out_ms * 24;
    if (to <= from || to - from <= in + out) {
        return {};
    }
    const std::size_t n = to - from;
    std::vector<char> o(pcm.begin() + static_cast<std::ptrdiff_t>(from * 2),
                        pcm.begin() + static_cast<std::ptrdiff_t>(to * 2));
    for (std::size_t i = 0; i < n; ++i) {
        float g = 1.0f;
        if (i < in && in > 1) {
            g = static_cast<float>(i) / static_cast<float>(in - 1);
        }
        if (i + out >= n && out > 1) {
            const float k = static_cast<float>(i - (n - out)) / static_cast<float>(out - 1);
            const float f = std::cos(1.5707963f * k);
            g *= f * f;
        }
        if (g == 1.0f) {
            continue;
        }
        const auto lo = static_cast<unsigned char>(o[i * 2]);
        const auto hi = static_cast<unsigned char>(o[i * 2 + 1]);
        const int bits = lo | (hi << 8);
        const float v = static_cast<float>(bits >= 32768 ? bits - 65536 : bits) * g;
        const auto q = static_cast<std::int16_t>(std::lround(std::clamp(v, -32768.0f, 32767.0f)));
        o[i * 2] = static_cast<char>(q & 0xFF);
        o[i * 2 + 1] = static_cast<char>((q >> 8) & 0xFF);
    }
    return o;
}

// The render tick (Game:Main) only: one more start of `clip` (PlayOut: it plays to its end), at
// `volume` (x its gain, 0..2) and `pitch` (0.5..2: lower is slower and deeper).
void Play(Clip clip, std::int64_t now, float volume = 1.0f, float pitch = 1.0f);
ClipAudio::Frame Latest(Clip clip);

class Mixer : public ClipAudio::Player {
public:
    explicit Mixer(Clip clip) : ClipAudio::Player({clip == Splash ? SplashGain : GulpGain}) {}
};

} // namespace VR::ItemAudio
