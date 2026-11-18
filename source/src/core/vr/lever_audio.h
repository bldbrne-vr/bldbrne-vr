// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include "core/vr/clip_audio.h"

// A lever pulled by hand that the game accepted (core/vr/bloodborne_levers.cpp): a heavy latch
// clunk, the game's own o990100001 (chosen in an audition, 2026-10-16), decoded at
// startup from the player's map banks (core/vr/game_sounds.cpp; nothing of the game is shipped).
// A hand pull skips the game's pull animation, whose TAE plays the lever's own sound, so without
// this a pull that worked was silent. It plays to its end (the recall mixer's 150 ms freshness
// would cut a 1.26 s clip short), once per accepted pull, never for a pull the game refused or a
// motion rehearsal.
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

// Its clunk to its end, once per accepted pull (the shared clip player, core/vr/clip_audio.h).
class Mixer {
public:
    void SetPcm(std::span<const char> pcm) {
        player_.SetPcm(pcm);
    }
    [[nodiscard]] bool HasPcm() const {
        return player_.HasPcm();
    }
    [[nodiscard]] bool Playing() const {
        return player_.Playing();
    }
    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             Frame state, std::int64_t now) {
        player_.Mix(buffer, frames, channels, rate, is_float,
                    {state.id, ClipAudio::PlayOut, state.ns}, now);
    }

private:
    ClipAudio::Player player_{{Gain, FreshNs}};
};

} // namespace VR::LeverAudio
