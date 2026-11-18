// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/clip_audio.h"

namespace VR::ToolEmptyAudio {
// One bounded voice, independent of firearm reload/empty sounds. Generated offline.
inline constexpr float Gain = .65f;
inline constexpr std::int64_t CooldownNs = 950'000'000;
class Cue {
public:
    ClipAudio::Frame Update(bool failed, bool enabled, std::int64_t now) {
        if (!enabled) {
            frame_.state = ClipAudio::Off;
        } else if (failed && now > 0 &&
                   (frame_.id == 0 || (now >= frame_.ns && now - frame_.ns >= CooldownNs))) {
            ++frame_.id;
            frame_.ns = now;
            frame_.state = ClipAudio::PlayOut;
        }
        return frame_;
    }
private:
    ClipAudio::Frame frame_{};
};
void Publish(bool failed, bool enabled, std::int64_t now);
ClipAudio::Frame Latest();
std::span<const char> Pcm();
class Mixer : public ClipAudio::Player {
public:
    Mixer() : ClipAudio::Player({Gain, 200'000'000, 200'000'000, .025f}) {}
};
} // namespace VR::ToolEmptyAudio
