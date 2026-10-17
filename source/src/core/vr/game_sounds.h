// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>

// The game's own recordings the VR mixers play (nothing of the game is in the repo): decoded once
// from the player's dump at startup on a worker thread (core/vr/fsb_sample.h), all from
// game asset, 24 kHz mono S16LE. Until it is done, or if it fails, each is empty and its
// mixer plays what it did before.
//  - the blades' grind loop (core/vr/grind_audio.h);
//  - the Beast Cutter's closing CHDING (core/vr/whip_audio.h MakeCutterClose).
namespace VR::GameSounds {

void LoadAsync(); // once; any thread after the game's files are mounted
std::span<const char> GrindLoop();
std::span<const char> CutterClose();

} // namespace VR::GameSounds
