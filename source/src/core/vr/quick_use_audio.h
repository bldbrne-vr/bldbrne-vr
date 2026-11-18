// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include "core/vr/clip_audio.h"
#include "core/vr/seqlock.h"

namespace VR::Quick {

struct UseAudioFrame {
    std::int64_t ns{};
    float progress{-1.0f}; // < 0: silent; 0..1: building toward use
    float volume{1.0f};
    std::uint8_t kind{0};  // the charging tool (UseAudioCallBeyond: its own voice too)
    bool full{false};      // ... gathered, waiting for its act
};

// Presenter is the sole writer; the main audio port is the sole playback owner.
inline SeqLock<UseAudioFrame>& UseAudioChannel() {
    static SeqLock<UseAudioFrame> channel;
    return channel;
}
inline void PublishUseAudio(float progress, float volume, std::int64_t now,
                            std::uint8_t kind = 0, bool full = false) {
    UseAudioChannel().Store({now, progress, volume, kind, full});
}
inline UseAudioFrame LatestUseAudio() {
    UseAudioFrame frame{};
    UseAudioChannel().Load(frame, 4);
    return frame;
}

// A Call Beyond's gathering, synthesized (the tester's 2026-11-04 "a better SFX as it charges and
// when it stiffens up and wants to be pushed up"): the stars answering. Gathering: a low drone
// rising a fifth and swelling, starlight glinting above it (bell-like, quicker and brighter as it
// gathers). Gathered: a held glassy fifth over a throb on the hand's pulse (0.62 s, QuickToolFeel)
// until it is raised (then the game's own call: CallSample). No assets, allocation or locks; one
// sample at a time, about 20 sines while it plays, nothing when silent.
class CallBeyondVoice {
public:
    // One sample (about -0.4 .. 0.4) at `dt` = 1 / rate: `progress` 0..1, `full` gathered.
    float Next(float progress, bool full, float dt) {
        constexpr float Tau = 6.2831853f;
        // The drone: A1 rising to E2, a slow glide, two detuned voices beating, two overtones.
        const float f0 = 55.0f * (1.0f + 0.5f * progress * progress);
        drone_hz_ = drone_hz_ > 0.0f ? drone_hz_ + (f0 - drone_hz_) * std::min(1.0f, dt * 25.0f)
                                     : f0;
        const float d = drone_hz_ * dt;
        a_ = Wrap(a_ + d), b_ = Wrap(b_ + d * 1.0045f), c_ = Wrap(c_ + 2.0f * d),
        e_ = Wrap(e_ + 3.006f * d);
        const float drone = 0.5f * std::sin(Tau * a_) + 0.5f * std::sin(Tau * b_) +
                            0.30f * std::sin(Tau * c_) + 0.12f * std::sin(Tau * e_);
        float swell = 0.30f + 0.70f * progress;
        if (full) {
            // The throb: a deep pulse each 0.62 s, struck and dying away (the hand's thrum); held
            // under the call to come (2026-11-06: lower "when we hold the hand").
            throb_ = Wrap(throb_ + 1.6129f * dt);
            swell = 0.35f + 0.45f * std::exp(-5.0f * throb_);
        }
        float out = drone * 0.062f * swell; // the low hum just under the rest (2026-11-06)
        // Starlight: bell-like glints from a bright pentatonic set, ever more as it gathers.
        const float per_s = full ? 5.0f : 2.0f + 22.0f * progress * progress;
        if (Random() < per_s * dt) {
            static constexpr std::array<float, 8> Scale{1318.51f, 1567.98f, 1760.0f, 2093.0f,
                                                        2349.32f, 2637.02f, 3135.96f, 3520.0f};
            Glint* quietest = &glints_[0];
            for (auto& g : glints_) {
                if (g.amp < quietest->amp) quietest = &g;
            }
            const auto pick = static_cast<std::size_t>(Random() * Scale.size()) % Scale.size();
            *quietest = {Scale[pick] * (0.997f + 0.006f * Random()), 0.0f,
                         (0.018f + 0.022f * Random()) * (0.45f + 0.55f * progress), 0.0f,
                         std::exp(-dt / (0.18f + 0.35f * Random()))};
        }
        for (auto& g : glints_) {
            if (g.amp < 1e-5f) continue;
            g.phase = Wrap(g.phase + g.hz * dt);
            g.attack = std::min(1.0f, g.attack + dt / 0.002f); // no click
            out += std::sin(Tau * g.phase) * g.amp * g.attack;
            g.amp *= g.decay;
        }
        // Held while gathered: a glassy fifth (E5, B5, E6) breathing, faded in and out, soft.
        hold_ += ((full ? 1.0f : 0.0f) - hold_) * std::min(1.0f, dt * 6.0f);
        if (hold_ > 1e-4f) {
            trem_ = Wrap(trem_ + 4.5f * dt);
            h1_ = Wrap(h1_ + 659.26f * dt), h2_ = Wrap(h2_ + 987.77f * dt),
            h3_ = Wrap(h3_ + 1318.51f * dt);
            const float breathe = 0.8f + 0.2f * std::sin(Tau * trem_);
            out += hold_ * breathe *
                   (0.010f * std::sin(Tau * h1_) + 0.008f * std::sin(Tau * h2_) +
                    0.004f * std::sin(Tau * h3_));
        }
        // All of it under the call (2026-11-06: "lower the volume of the first part (the
        // chime and low hum)", then with the game's own call: "raise the volume of our own
        // 'intro' sound of it").
        return out * 0.75f;
    }

private:
    struct Glint {
        float hz{}, phase{}, amp{}, attack{}, decay{};
    };
    static float Wrap(float p) { return p - std::floor(p); }
    float Random() {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<float>(seed_ >> 8) / 16777216.0f;
    }
    std::array<Glint, 8> glints_{};
    float drone_hz_{}, a_{}, b_{}, c_{}, e_{}, throb_{}, hold_{}, trem_{}, h1_{}, h2_{}, h3_{};
    std::uint32_t seed_{0x2545F491u};
};
// UseAudioFrame::kind: the charging tool's ToolFeel (core/vr/quick_items.h; checked there).
inline constexpr std::uint8_t UseAudioCallBeyond = 5;

// A Call Beyond called (2026-11-06: "can we have a sound playing when it gets put up in
// the air? (do not add another mixer entry ...)"): + 1 per confirmed direct use (quick_items.h,
// the presenter); UseAudioMixer plays the call once for each new one.
inline std::atomic<std::uint32_t> beyond_raises{0};

// The call: the game's own A Call Beyond sound (its use's effect 200001 plays it; a direct use has
// no animation, so none of it), cut in where the game's own use launches its stars, 1.0 s in (the
// user's pick, 2026-11-06, over our synthesized chime: "I like that way more ... where the sound
// is cut"). Decoded at startup from the player's main bank (core/vr/game_sounds.cpp; nothing of
// the game is shipped), faded in over 30 ms, played by UseAudioMixer's own clip player; silent
// until decoded.
inline constexpr const char* CallSample = "s000200001";
inline constexpr std::size_t CallFromMs = 1000, CallFadeInMs = 30, CallFadeOutMs = 200;
inline constexpr float CallGain = 0.5f;

// A soft band-limited air sound, generated without assets, allocations or locks in playback.
// Pressure/hold progress raises both loudness and brightness. Smooth attack and a 15 ms release
// leave the game's completion sound clear; stale input cannot leave a whoosh running in menus.
// Mixed before backend volume/mute, on front L/R for both SDL and OpenAL (including 7.1).
class UseAudioMixer {
public:
    // The game's call (CallSample), once decoded (24 kHz mono S16LE); never swapped while it plays.
    void SetCallPcm(std::span<const char> pcm) { call_.SetPcm(pcm); }
    [[nodiscard]] bool HasCallPcm() const { return call_.HasPcm(); }

    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             UseAudioFrame state, std::int64_t now) {
        if (!buffer || !frames || rate < 8000 ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const bool fresh = state.ns > 0 && now >= state.ns && now - state.ns < 150'000'000;
        const bool active = fresh && std::isfinite(state.progress) && state.progress >= 0 &&
                            std::isfinite(state.volume) && state.volume > 0;
        const float progress = active ? std::clamp(state.progress, 0.0f, 1.0f) : progress_;
        // A Call Beyond: its own voice over the air, the air lower under it (lower still once
        // gathered: the chord and the throb carry it).
        const bool beyond = active && state.kind == UseAudioCallBeyond;
        const float air = beyond ? (state.full ? 0.20f : 0.40f) : 1.0f;
        const float target = active ? (0.08f + 0.18f * progress * progress) * air *
                                          std::clamp(state.volume, 0.0f, 2.0f)
                                    : 0.0f;
        beyond_target_ = beyond ? std::clamp(state.volume, 0.0f, 2.0f) : 0.0f;
        beyond_full_ = beyond && state.full;
        // A new call: the game's call once, at the use sounds' volume (none before the first look),
        // played to its end.
        if (fresh && std::isfinite(state.volume)) volume_ = std::clamp(state.volume, 0.0f, 2.0f);
        const std::uint32_t raises = beyond_raises.load(std::memory_order_acquire);
        if (raises != raises_seen_ && raises_known_ && volume_ > 0) {
            call_frame_ = {call_frame_.id + 1, ClipAudio::PlayOut, now, volume_, 1.0f};
        }
        raises_seen_ = raises, raises_known_ = true;
        call_.Mix(buffer, frames, channels, rate, is_float, call_frame_, now);
        if (target == 0 && gain_ == 0 && beyond_target_ == 0 && beyond_gain_ == 0) return;
        dt_ = 1.0f / static_cast<float>(rate);
        beyond_release_ = 1.0f / (0.08f * rate); // released within 80 ms
        const float smoothing = 1.0f - std::exp(-1.0f / (0.015f * rate));
        const float release = 0.52f / (0.015f * rate); // even maximum volume stops within 15 ms
        const float highpass = 1.0f - std::exp(-6.2831853f * 150.0f / rate);
        // Smoothed coefficients keep pressure changes from introducing clicks.
        const float cutoff = std::min(450.0f + 2750.0f * progress, 0.4f * rate);
        const float lowpass = 1.0f - std::exp(-6.2831853f * cutoff / rate);
        progress_ = progress;
        if (is_float) {
            MixSamples(static_cast<float*>(buffer), frames, channels, target, lowpass,
                       highpass, smoothing, release);
        } else {
            MixSamples(static_cast<std::int16_t*>(buffer), frames, channels, target, lowpass,
                       highpass, smoothing, release);
        }
    }

private:
    template <class T> static void Add(T& destination, float voice) {
        if (voice == 0) return;
        if constexpr (std::is_same_v<T, float>) {
            destination = std::clamp(destination + voice, -1.0f, 1.0f);
        } else {
            destination = static_cast<T>(std::clamp(
                std::lround(destination + voice * 32768.0f), -32768L, 32767L));
        }
    }
    template <class T>
    void MixSamples(T* output, std::size_t frames, unsigned channels, float target,
                    float lowpass, float highpass, float smoothing, float release) {
        for (std::size_t i = 0; i < frames; ++i) {
            gain_ = target > 0 ? gain_ + (target - gain_) * smoothing
                              : std::max(0.0f, gain_ - release);
            beyond_gain_ = beyond_target_ > 0
                               ? beyond_gain_ + (beyond_target_ - beyond_gain_) * smoothing
                               : std::max(0.0f, beyond_gain_ - beyond_release_);
            if (beyond_gain_ > 0) {
                beyond_live_ = true;
            } else if (beyond_live_ && beyond_target_ == 0) {
                beyond_ = {}; // faded out: the next gathering starts afresh
                beyond_live_ = false;
            }
            if (gain_ == 0) {
                low_ = bass_ = 0;
                if (beyond_gain_ == 0) break;
            }
            float voice = 0.0f;
            if (gain_ > 0) {
                // Deterministic, independent of the game's random numbers.
                noise_ ^= noise_ << 13;
                noise_ ^= noise_ >> 17;
                noise_ ^= noise_ << 5;
                const float noise = static_cast<float>(noise_ >> 8) / 8388608.0f - 1.0f;
                lowpass_ += (lowpass - lowpass_) * smoothing;
                low_ += lowpass_ * (noise - low_);
                bass_ += highpass * (low_ - bass_);
                voice = (low_ - bass_) * gain_;
            }
            if (beyond_gain_ > 0) {
                voice += beyond_.Next(progress_, beyond_full_, dt_) * beyond_gain_;
            }
            // A gentle left bias matches the item hand; never put this cue in LFE/rear channels.
            Add(output[i * channels], voice * (channels == 1 ? 1.0f : 0.8f));
            if (channels > 1) Add(output[i * channels + 1], voice * 0.6f);
        }
    }
    std::uint32_t noise_{0x9e3779b9};
    float gain_{}, progress_{}, low_{}, bass_{}, lowpass_{};
    CallBeyondVoice beyond_{};
    float beyond_gain_{}, beyond_target_{}, beyond_release_{}, dt_{};
    bool beyond_full_{false}, beyond_live_{false};
    ClipAudio::Player call_{ClipAudio::Config{CallGain}};
    ClipAudio::Frame call_frame_{};
    float volume_{1.0f};
    std::uint32_t raises_seen_{};
    bool raises_known_{false};
};
} // namespace VR::Quick
