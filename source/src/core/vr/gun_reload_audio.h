// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/seqlock.h"
#include "core/vr/whip_audio.h"
namespace VR::Gun {
struct ReloadAudioFrame {
    Whip::RecallAudioFrame reload{}, click{};
};
enum class ReloadCue { None, Reload, Empty };
void PublishReloadAudio(ReloadCue cue, bool enabled, std::int64_t now);
ReloadAudioFrame LatestReloadAudio();
std::span<const char> ReloadPcm();
std::span<const char> EmptyPcm();
class ReloadAudioMixer {
public:
    // x1.75 (about +5 dB) over the whip clink's level (2026-10-14, the user, after the clips'
    // own x1.4, which already reach full scale; the mix clamps).
    static constexpr float Gain = 0.84f;
    ReloadAudioMixer(std::span<const char> reload, std::span<const char> click)
        : reload_({}, reload, Gain), click_({}, click, Gain) {}
    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             ReloadAudioFrame state, std::int64_t now) {
        reload_.Mix(buffer, frames, channels, rate, is_float, state.reload, now);
        click_.Mix(buffer, frames, channels, rate, is_float, state.click, now);
    }
private:
    Whip::RecallAudioMixer reload_, click_;
};
} // namespace VR::Gun
