// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include "core/vr/clip_audio.h"

// The Madaras Whistle blown at the mouth (core/vr/quick_items.h Config::whistle_s; the user's
// 2026-10-30 design): its call plays from the moment it is blown and stops when the trigger is let
// go (or the whistle leaves the mouth) before the hold completes; held through it, the snake comes.
// The hold completes 100 ms before the audible call ends; the remaining sound plays out. The call
// is the game's own hebi_hue (event s202052's only sound, 1.06 s, 48 kHz in the bank), decoded at
// startup from the player's sprj_main_eng.fsb (core/vr/game_sounds.cpp; nothing of the game is
// shipped). Played by us rather than by the game's PlaySound, so it can be stopped; the use then
// asks the game for no sound (Request::voiced).
namespace VR::WhistleAudio {

constexpr const char* SampleName = "hebi_hue";
constexpr float Gain = 0.6f;
// Frame::state (Quick::WhistleState's values).
constexpr std::uint8_t Off = ClipAudio::Off;         // stopped (or none): a call playing fades
constexpr std::uint8_t Blowing = ClipAudio::Playing; // being blown: a new id starts the call
constexpr std::uint8_t Done = ClipAudio::PlayOut;    // hold completed: the call plays out
// A blow heard later than this (a stall, the audio starting) is not started; a call still being
// blown but not published again for this long (the presenter stopped) fades out.
constexpr std::int64_t StartFreshNs = 300'000'000;
constexpr std::int64_t HeldFreshNs = 200'000'000;
constexpr float FadeS = 0.03f; // stopped: out over this (no click)

using Frame = ClipAudio::Frame; // id: + 1 per blow

// The presenter, every frame.
void Publish(std::uint64_t id, std::uint8_t state, std::int64_t now);
Frame Latest();

// The audible call's length (24 kHz mono S16LE): to the end of its last 10 ms that is
// within 30 dB of its loudest (the rest is not heard over the game). 0: none.
inline float CallLength(std::span<const char> pcm, unsigned rate = 24000) {
    const std::size_t count = pcm.size() / 2;
    const std::size_t window = rate / 100;
    if (window == 0 || count < window) {
        return 0.0f;
    }
    const auto energy = [&](std::size_t start) {
        double sum = 0.0;
        for (std::size_t i = start; i < start + window; ++i) {
            const int bits = static_cast<unsigned char>(pcm[i * 2]) |
                             (static_cast<unsigned char>(pcm[i * 2 + 1]) << 8);
            const double s = (bits >= 32768 ? bits - 65536 : bits) / 32768.0;
            sum += s * s;
        }
        return sum;
    };
    double loudest = 0.0;
    for (std::size_t w = 0; w + window <= count; w += window) {
        loudest = std::max(loudest, energy(w));
    }
    if (loudest <= 0.0) {
        return 0.0f;
    }
    std::size_t end = 0;
    for (std::size_t w = 0; w + window <= count; w += window) {
        if (energy(w) >= loudest * 1e-3) { // -30 dB
            end = w + window;
        }
    }
    return static_cast<float>(end) / static_cast<float>(rate);
}

// Computed once at sound load. Preserve the missing/silent-call fallback (0), and keep any
// positive clip on the cancellable whistle path even if it is shorter than the 100 ms lead.
constexpr float RequiredHoldS(float audible_s) {
    return audible_s > 0.0f ? std::max(0.01f, audible_s - 0.10f) : 0.0f;
}

// The call (the shared clip player, core/vr/clip_audio.h; centred: the whistle is at the mouth).
class Mixer : public ClipAudio::Player {
public:
    Mixer() : ClipAudio::Player({Gain, StartFreshNs, HeldFreshNs, FadeS}) {}
};

} // namespace VR::WhistleAudio
