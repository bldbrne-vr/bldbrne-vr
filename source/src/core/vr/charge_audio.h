// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include "core/vr/clip_audio.h"
#include "core/vr/item_audio.h"

// A melee attack held to its full charge: a short ring of the game's own, with the haptics' three
// rings (2026-11-08: no glow - "we have haptic feedback already ... so maybe we add some
// sound"). The game's charge flash (sprj_main tame_light00: a bright ting, its tail cut), decoded
// once at startup from the player's bank (core/vr/game_sounds.cpp; nothing of the game is
// shipped), started by the presenter's full charge, played to its end by the shared clip player
// (core/vr/clip_audio.h), centred into the main port, quietly.
namespace VR::ChargeAudio {

constexpr ItemAudio::CutSpec ReadyCut{"sprj_tame_light00", 0, 820, 0, 220};
constexpr float Gain = 0.45f;

// The presenter's frame thread only: one more start (PlayOut: it plays to its end).
void Play(std::int64_t now);
ClipAudio::Frame Latest();

class Mixer : public ClipAudio::Player {
public:
    Mixer() : ClipAudio::Player({Gain}) {}
};

} // namespace VR::ChargeAudio
