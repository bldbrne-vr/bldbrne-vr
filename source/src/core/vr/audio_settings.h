// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
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

// audio_lead_ms (the OpenAL output, binaural or not): how much of the game's sound waits in the
// queue ahead of the speakers. OpenAL mixes ~20 ms at a time, so a queue of 2-3 of the game's
// 5.3 ms buffers ran dry many times a minute on a busy CPU, each gap a pop (the user's report,
// 2026-11-26; bbport fixed the same crackle on Windows with an 80 ms lead and its audio threads at
// the highest priority). The output then takes each buffer as soon as the game writes it until
// the queue holds this much, and its thread runs at the highest priority. 0 = the old timing (a
// 5.3 ms clock, about 10-16 ms queued, normal priority). Sound plays the difference later than
// before (40: about 25-30 ms).
constexpr int DefaultLeadMs = 40;
constexpr int MaxLeadMs = 200;
inline std::atomic<int> g_lead_ms{DefaultLeadMs};

[[nodiscard]] inline bool Binaural() {
    return g_binaural.load(std::memory_order_relaxed);
}

[[nodiscard]] inline int LeadMs() {
    return g_lead_ms.load(std::memory_order_relaxed);
}

inline bool ParseAudioSetting(const std::string& key, const std::string& value) {
    if (key == "audio_binaural") {
        g_binaural.store(value == "1" || value == "true", std::memory_order_relaxed);
        return true;
    }
    if (key == "audio_lead_ms") {
        char* end = nullptr;
        const long ms = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str()) {
            g_lead_ms.store(static_cast<int>(std::clamp<long>(ms, 0, MaxLeadMs)),
                            std::memory_order_relaxed);
        }
        return true;
    }
    return false;
}

inline void WriteAudioDefaults(std::ostream& out) {
    out << "# sound: 1 = binaural (the game's 7.1 mix through an HRTF, sounds behind you sound "
           "behind you;\n"
        << "# for the headset's speakers or headphones), 0 = the emulator's audio settings "
           "(stereo fold)\n"
        << "audio_binaural=1\n"
        << "# sound queued ahead of the speakers, ms: more stops pops on a busy CPU, less plays "
           "sooner\n# (0 = the old timing; 0-200)\n"
        << "audio_lead_ms=" << DefaultLeadMs << "\n";
}

} // namespace VR::Sound
