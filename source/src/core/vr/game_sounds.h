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
//  - a lever pulled by hand: its clunk (core/vr/lever_audio.h, from a map bank).
namespace VR::GameSounds {

void LoadAsync(); // once; any thread after the game's files are mounted
std::span<const char> GrindLoop();
std::span<const char> CutterClose();
std::span<const char> LeverClunk();

} // namespace VR::GameSounds
