// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

// One game sound played by us, shared by the lever's clunk (core/vr/lever_audio.h) and the
// Madaras Whistle's call (core/vr/whistle_audio.h): 24 kHz mono S16LE decoded at startup from the
// player's own banks (core/vr/game_sounds.cpp; nothing of the game is shipped), started by its
// owner's new id, played to its end or stopped with a short fade (no click). Allocation-free,
// mixed into the main port before the backend's volume, centred. Silent, a Mix is a few
// compares.
namespace VR::ClipAudio {

// Frame::state.
constexpr std::uint8_t Off = 0;     // stopped (or none): one playing fades out
constexpr std::uint8_t Playing = 1; // held on: a new id starts it, published on or it fades out
constexpr std::uint8_t PlayOut = 2; // a new id starts it; it plays to its end whatever follows

struct Frame {
    std::uint64_t id{0}; // + 1 per start
    std::uint8_t state{Off};
    std::int64_t ns{0}; // when it was published (steady clock)
    float volume{1.0f}; // this start's, x Config::gain (0..2)
    float pitch{1.0f};  // ... played this much faster (0.5..2: lower is slower and deeper)
};

struct Config {
    float gain{0.6f};
    // A start heard further than this from its time (a stall, the audio starting) is not played.
    std::int64_t start_fresh_ns{300'000'000};
    // Playing, not published again for this long (its owner stopped): it fades out.
    std::int64_t held_fresh_ns{200'000'000};
    float fade_s{0.03f}; // stopped: out over this
};

class Player {
public:
    explicit Player(Config c = {}) : c_(c) {}

    // Never swapped while it plays.
    void SetPcm(std::span<const char> pcm) {
        if (!live_ && pcm.size() >= 4) {
            pcm_ = pcm;
        }
    }
    [[nodiscard]] bool HasPcm() const {
        return pcm_.size() >= 4;
    }
    [[nodiscard]] bool Playing() const {
        return live_;
    }

    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             Frame state, std::int64_t now) {
        if (state.id != id_) {
            id_ = state.id;
            const std::int64_t age = now - state.ns;
            if (HasPcm() && state.state != Off && state.ns > 0 && age < c_.start_fresh_ns &&
                -age < c_.start_fresh_ns) {
                pos_ = 0.0;
                gain_ = 1.0f;
                volume_ = std::isfinite(state.volume) ? std::clamp(state.volume, 0.0f, 2.0f) : 1.0f;
                pitch_ = std::isfinite(state.pitch) ? std::clamp(state.pitch, 0.5f, 2.0f) : 1.0f;
                fading_ = false;
                live_ = true;
            }
        }
        if (live_ && (state.state == Off ||
                      (state.state == ClipAudio::Playing && now - state.ns >= c_.held_fresh_ns))) {
            fading_ = true; // stopped before its end (or nobody says it is still on)
        }
        if (!live_ || !buffer || !frames || !rate ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const double step = 24000.0 / rate * pitch_;
        const float fade = 1.0f / (c_.fade_s * static_cast<float>(rate));
        if (is_float) {
            MixSamples(static_cast<float*>(buffer), frames, channels, step, fade);
        } else {
            MixSamples(static_cast<std::int16_t*>(buffer), frames, channels, step, fade);
        }
    }

private:
    static float Sample(std::span<const char> pcm, std::size_t index) {
        const auto lo = static_cast<unsigned char>(pcm[index * 2]);
        const auto hi = static_cast<unsigned char>(pcm[index * 2 + 1]);
        const int bits = lo | (hi << 8);
        return static_cast<float>(bits >= 32768 ? bits - 65536 : bits) / 32768.0f;
    }
    template <class T>
    static void Add(T& destination, float voice) {
        if constexpr (std::is_same_v<T, float>) {
            destination = std::clamp(destination + voice, -1.0f, 1.0f);
        } else {
            destination = static_cast<T>(
                std::clamp(std::lround(destination + voice * 32768.0f), -32768L, 32767L));
        }
    }
    template <class T>
    void MixSamples(T* output, std::size_t frames, unsigned channels, double step, float fade) {
        const std::size_t count = pcm_.size() / 2;
        // Centred: equal power on the front left and right (common to both console layouts).
        const float side = channels == 1 ? 1.0f : std::sqrt(0.5f);
        for (std::size_t i = 0; i < frames && live_; ++i) {
            const auto a = static_cast<std::size_t>(pos_);
            if (a >= count) {
                live_ = false;
                break;
            }
            if (fading_) {
                gain_ -= fade;
                if (gain_ <= 0.0f) {
                    live_ = false;
                    break;
                }
            }
            const std::size_t b = a + 1 < count ? a + 1 : a;
            const float s = Sample(pcm_, a) +
                            (Sample(pcm_, b) - Sample(pcm_, a)) * static_cast<float>(pos_ - a);
            const float voice = s * c_.gain * volume_ * gain_ * side;
            Add(output[i * channels], voice);
            if (channels > 1) {
                Add(output[i * channels + 1], voice);
            }
            pos_ += step;
        }
    }

    Config c_;
    std::span<const char> pcm_{};
    std::uint64_t id_{0};
    double pos_{0.0};
    float gain_{1.0f};
    float volume_{1.0f};
    float pitch_{1.0f};
    bool fading_{false};
    bool live_{false};
};

} // namespace VR::ClipAudio
