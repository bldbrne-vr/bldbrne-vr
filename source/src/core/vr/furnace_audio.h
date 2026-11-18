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
#include "core/vr/grind_audio.h"
#include "core/vr/item_audio.h"

// The Boom Hammer's furnace started like an engine (core/vr/trick_weapons.h GripUse::Pull; the
// user's 2026-11-06 "a flintrock or something, a 'CHUFF' sound 'CHURFF GRRR'"). By the user's
// audition picks ("A8_flint_then_whoomph ... lower its pitch a little bit ... mix it with a pitch
// shifted A5_flintlock_snap ... alternate between the 2", then the R4 pattern with R5's pitch):
//  - Snap: the musket's dry flint snap, each pull that does not catch;
//  - Whoomph: the snap with the Hunter's Torch's swung "fwomp" 30 ms after it, the pull that
//    catches (and, quieter, each squeeze that pumps it up);
// both about 80% pitch (lower and slower), a few percent higher or lower each pull (PullPitch);
//  - Growl: the Flamesprayer's roar looped while it idles, eased by the blades' grind mixer
//    (core/vr/grind_audio.h GrindMixer: louder, higher and brighter as it revs).
// All the game's own recordings from the main bank, cut at startup from the player's files
// (core/vr/game_sounds.cpp; nothing of the game is shipped); until decoded, it is felt only.
namespace VR::FurnaceAudio {

enum Clip : std::uint8_t { Snap = 0, Whoomph = 1, ClipCount = 2 };

constexpr ItemAudio::CutSpec SnapCut{"sprj_musket1", 0, 115, 0, 30};    // before its bang
constexpr ItemAudio::CutSpec SwingCut{"taimatsu_swing", 30, 430, 5, 150};
constexpr std::size_t SwingAtMs = 30; // the fwomp after the snap ...
constexpr float SwingMix = 0.7f;      // ... this loud
// The growl's loop: its part of the recording, the seam folded over GrowlFade samples (40 ms:
// clean on this one; the audition's seam check).
struct LoopSpec {
    const char* sample;
    std::size_t from_ms, to_ms;
};
constexpr LoopSpec GrowlLoop{"kaenhoushaki_loop", 1170, 3073};
constexpr std::size_t GrowlFade = 960;
// About 80% pitch, PitchJitter either way each pull.
constexpr float SnapPitch = 0.8f, WhoomphPitch = 0.8f, PitchJitter = 0.05f;
// The recordings are hot (6-7 dB over the audition's level).
constexpr float SnapGain = 0.45f, WhoomphGain = 0.5f;

// The snap and the swing, cut: the Whoomph (as long as the longer of them).
inline std::vector<char> MakeWhoomph(std::span<const char> snap, std::span<const char> swing) {
    const std::size_t a = snap.size() / 2, b = swing.size() / 2, at = SwingAtMs * 24;
    if (a == 0 || b == 0) {
        return {};
    }
    const std::size_t n = std::max(a, at + b);
    const auto sample = [](std::span<const char> pcm, std::size_t i) {
        const int bits = static_cast<unsigned char>(pcm[2 * i]) |
                         (static_cast<unsigned char>(pcm[2 * i + 1]) << 8);
        return static_cast<float>(bits >= 32768 ? bits - 65536 : bits);
    };
    std::vector<char> out(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        float v = i < a ? sample(snap, i) : 0.0f;
        if (i >= at && i - at < b) {
            v += SwingMix * sample(swing, i - at);
        }
        const auto q = static_cast<std::int16_t>(std::lround(std::clamp(v, -32768.0f, 32767.0f)));
        out[2 * i] = static_cast<char>(q & 0xFF);
        out[2 * i + 1] = static_cast<char>((q >> 8) & 0xFF);
    }
    return out;
}

// The presenter's grip tick (the only writer): one more start of `clip` at `volume` (0..2) and
// `pitch` (0.5..2).
void Play(Clip clip, std::int64_t now, float volume, float pitch);
// `base` a few percent higher or lower (PitchJitter), each call another.
float PullPitch(float base);
ClipAudio::Frame Latest(Clip clip);
// ... and, every tick of the Boom Hammer in the hand, the growl's level (0: silent).
void PublishGrowl(const Grind::GrindFrame& frame);
Grind::GrindFrame LatestGrowl();

class Mixer : public ClipAudio::Player {
public:
    explicit Mixer(Clip clip) : ClipAudio::Player({clip == Snap ? SnapGain : WhoomphGain}) {}
};

} // namespace VR::FurnaceAudio
