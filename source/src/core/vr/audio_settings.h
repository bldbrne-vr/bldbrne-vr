// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <atomic>
#include <ostream>
#include <string>

// Sound in VR: its vr.ini key. Parsed by VR::LoadSettings, which hands every key it does not know
// to ParseAudioSetting, and written with the other defaults when vr.ini is created. Read once, when
// the game opens its audio (sceAudioOutInit): a change applies from the next start.
//
// What the game gives us: Bloodborne always mixes 7.1 (FMOD; no speaker / headphone option in its
// menus), and its 3D listener is the render camera, which the VR camera hook drives from your head
// (position and orientation), so the mix already turns with your head. A headset over Link or
// Air Link is a stereo endpoint, so that 7.1 mix has to become two ears:
//   audio_binaural=1 (the default since 2026-10-13): OpenAL Soft renders the eight channels as
//     virtual speakers around your head through its HRTF (binaural: behind sounds behind, sides
//     at the ear), the "headphones" choice for a headset's near-ear speakers or headphones.
//   audio_binaural=0: the emulator's own audio settings as before (default SDL: the console's
//     7.1 -> stereo fold, where a sound behind you is only a quieter one in front).
namespace VR::Sound {

inline std::atomic<bool> g_binaural{true};

[[nodiscard]] inline bool Binaural() {
    return g_binaural.load(std::memory_order_relaxed);
}

inline bool ParseAudioSetting(const std::string& key, const std::string& value) {
    if (key == "audio_binaural") {
        g_binaural.store(value == "1" || value == "true", std::memory_order_relaxed);
        return true;
    }
    return false;
}

inline void WriteAudioDefaults(std::ostream& out) {
    out << "# sound: 1 = binaural (the game's 7.1 mix through an HRTF, sounds behind you sound "
           "behind you;\n"
        << "# for the headset's speakers or headphones), 0 = the emulator's audio settings "
           "(stereo fold)\n"
        << "audio_binaural=1\n";
}

} // namespace VR::Sound
