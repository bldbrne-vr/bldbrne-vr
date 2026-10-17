// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::AudioOut {

struct PortOut;

class PortBackend {
public:
    virtual ~PortBackend() = default;

    /// Guaranteed to be called in intervals of at least port buffer time,
    /// with size equal to port buffer size.
    virtual void Output(void* ptr) = 0;

    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

class OpenALAudioOut final : public AudioOutBackend {
public:
    // `binaural`: every port renders HRTF stereo (VR, core/vr/audio_settings.h), whatever the
    // emulator's openal_hrtf / openal_output_mode settings say.
    explicit OpenALAudioOut(bool binaural = false);
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

} // namespace Libraries::AudioOut
