// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The ladder skip's sounds (cinematic_ladder_skip; the tester, 2026-11-06): behind the black the
// hunter climbs at cinematic_ladder_skip_speed times the game's pace, and his climbing clips'
// sounds came that much faster too (distracting). Of his own sounds while the climb is
// fast-forwarded only every round(rate)-th plays: his steps keep the rhythm he climbs at. The
// world's sounds (an enemy, the wind) are left alone. Pure: unit-tested in tests/test_cinematic.cpp;
// the PlaySound detour is bloodborne_stagger.cpp's (LadderSoundAt).
namespace VR::LadderSteps {

// A sound this close to his feet (game metres) is his own: the rungs his hands and feet take.
constexpr float NearM = 2.5f;

// The climb fast-forwarded: the skip's black, his climb going on by itself, faster than the game.
constexpr bool Fast(bool black, int climb, float rate) {
    return black && climb != 0 && rate > 1.5f;
}

// His `index`-th sound (0 = the first) since the climb went fast, at `rate`: whether it plays.
constexpr bool Keep(u32 index, float rate) {
    const u32 every = rate > 1.0f ? static_cast<u32>(rate + 0.5f) : 1u;
    return every <= 1u || index % every == 0;
}

// Whether a sound at `pos` is at his feet (`feet`, game metres).
constexpr bool Near(const float* pos, const float* feet) {
    const float dx = pos[0] - feet[0], dy = pos[1] - feet[1], dz = pos[2] - feet[2];
    return dx * dx + dy * dy + dz * dz < NearM * NearM;
}

} // namespace VR::LadderSteps
