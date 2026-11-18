// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <span>

// The game's own recordings the VR mixers play (nothing of the game is in the repo): decoded once
// from the player's dump at startup on a worker thread (core/vr/fsb_sample.h), all from
// sprj_main_eng.fsb, 24 kHz mono S16LE. Until it is done, or if it fails, each is empty and its
// mixer plays what it did before.
//  - the blades' grind loop (core/vr/grind_audio.h);
//  - the Beast Cutter's closing CHDING (core/vr/whip_audio.h MakeCutterClose);
//  - a lever pulled by hand: its clunk (core/vr/lever_audio.h, from a map bank);
//  - the Madaras Whistle's call (core/vr/whistle_audio.h), and how long it is blown;
//  - the squeezed Augur's blood splash and the swallow, cut short (core/vr/item_audio.h Cut);
//  - the Boom Hammer's furnace started by hand: its flint snap, the snap with a fwomp, its
//    growl's loop (core/vr/furnace_audio.h);
//  - A Call Beyond raised: the game's own call from where its stars launch
//    (core/vr/quick_use_audio.h CallSample);
//  - a melee attack held to its full charge: the game's charge flash, cut short
//    (core/vr/charge_audio.h);
//  - Tonitrus charged: a thunderclap layered from the game's own (core/vr/thunder_audio.h).
namespace VR::GameSounds {

void LoadAsync(); // once; any thread after the game's files are mounted
std::span<const char> GrindLoop();
std::span<const char> CutterClose();
std::span<const char> LeverClunk();
std::span<const char> WhistleCall();
float WhistleCallS(); // Required mouth hold (audible call minus 100 ms); 0 until decoded
std::span<const char> AugurSplash();
std::span<const char> Gulp();
std::span<const char> FurnaceSnap();
std::span<const char> FurnaceWhoomph();
std::span<const char> FurnaceGrowl();
std::span<const char> BeyondCall();
std::span<const char> ChargeReady();
std::span<const char> ThunderClap();

} // namespace VR::GameSounds
