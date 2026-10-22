// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

// The blades' grind (vr.ini blade_grind; the tester's 2026-10-14 pick of the auditions "A" and "B"):
// one blade drawn along the other grinds on as long as it slides, louder and brighter the faster
// it goes, silent the moment it stops (core/vr/blade_sounds.h GrindLevel). The sound is the game's
// own sparks loop (sprj_main_eng.fsb w_cs_spark_loop, the Whirligig Saw's), read from the player's
// game files and decoded at startup (core/vr/fsb_sample.h); the katana's shing still marks each
// stroke (the game's PlaySound). Mixed into the first main audio port, as the whip's drag.
namespace VR::Grind {

constexpr const char* SampleName = "w_cs_spark_loop";
constexpr float Gain = 0.32f;          // at full level
constexpr float LowPitch = 0.8f;       // blade_grind=2: 20% lower and heavier ("B")
constexpr float DarkHz = 1500.0f;      // the lowpass when barely sliding ...
constexpr float BrightHz = 10500.0f;   // ... and at full speed
constexpr std::size_t LoopFade = 480;  // samples (20 ms at 24 kHz): the loop's seam crossfaded

struct GrindFrame {
    std::int64_t ns{0};
    float level{0.0f}; // 0..1 (BladeSound::GrindLevel)
    float pan{0.0f};
    float pitch{1.0f};
    float volume{1.0f};
};

// Single game-thread writer (bloodborne_body.cpp BladeSounds); the first main port reads.
void PublishGrind(const GrindFrame& frame);
GrindFrame LatestGrind();
// The loop (core/vr/game_sounds.h, decoded at startup). Until it is there, or if it never is, it
// is empty and the strokes ring on as before (BladeSound::Director).
std::span<const char> GrindPcm();
bool GrindReady();

// The decoded loop (24 kHz mono S16LE) with its seam crossfaded: the last LoopFade samples folded
// into the first, equal power (the sparks are noise-like). Too short a clip: unchanged.
inline std::vector<char> MakeSeamless(const std::vector<char>& pcm) {
    const std::size_t n = pcm.size() / 2;
    if (n < 4 * LoopFade) {
        return pcm;
    }
    const auto at = [&](std::size_t i) {
        return static_cast<float>(static_cast<std::int16_t>(
            static_cast<std::uint16_t>(static_cast<unsigned char>(pcm[2 * i]) |
                                       (static_cast<unsigned char>(pcm[2 * i + 1]) << 8))));
    };
    const std::size_t m = n - LoopFade;
    std::vector<char> out(m * 2);
    for (std::size_t i = 0; i < m; ++i) {
        float v = at(i);
        if (i < LoopFade) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(LoopFade);
            v = v * std::sin(1.5707963f * u) + at(m + i) * std::cos(1.5707963f * u);
        }
        const auto q = static_cast<std::int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
        out[2 * i] = static_cast<char>(q & 0xFF);
        out[2 * i + 1] = static_cast<char>((q >> 8) & 0xFF);
    }
    return out;
}

// Allocation-free: the loop at the frame's level, its gain eased (AttackS up, ReleaseS down), a
// one-pole lowpass following the eased level (darker slow, brighter fast). A frame older than
// StaleNs is silence (the game paused, the body stopped publishing).
class GrindMixer {
public:
    static constexpr float AttackS = 0.015f;
    static constexpr float ReleaseS = 0.06f;
    static constexpr std::int64_t StaleNs = 150'000'000;

    explicit GrindMixer(std::span<const char> pcm = {}) : pcm_(pcm) {}
    void SetPcm(std::span<const char> pcm) { pcm_ = pcm; }
    bool HasPcm() const { return pcm_.size() >= 4; }

    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             GrindFrame state, std::int64_t now) {
        if (!buffer || !frames || !rate || !HasPcm() ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const bool fresh = state.ns > 0 && now >= state.ns && now - state.ns < StaleNs;
        float target = fresh && std::isfinite(state.level) ? std::clamp(state.level, 0.0f, 1.0f)
                                                           : 0.0f;
        const float volume =
            std::isfinite(state.volume) ? std::clamp(state.volume, 0.0f, 2.0f) : 1.0f;
        if (target > 0.0f && gain_ == 0.0f) {
            // From silence: somewhere new in the loop (no start that is always the same).
            seed_ = seed_ * 1664525u + 1013904223u;
            pos_ = static_cast<double>(seed_ % static_cast<std::uint32_t>(pcm_.size() / 2));
        }
        if (target == 0.0f && gain_ == 0.0f) {
            return;
        }
        const float pan = std::isfinite(state.pan) ? std::clamp(state.pan, -1.0f, 1.0f) : 0.0f;
        const float left = std::sqrt(0.5f * (1.0f - pan));
        const float right = std::sqrt(0.5f * (1.0f + pan));
        const float pitch =
            std::isfinite(state.pitch) ? std::clamp(state.pitch, 0.5f, 2.0f) : 1.0f;
        const double step = 24000.0 / rate * pitch;
        const float attack = 1.0f - std::exp(-1.0f / (AttackS * static_cast<float>(rate)));
        const float release = 1.0f - std::exp(-1.0f / (ReleaseS * static_cast<float>(rate)));
        // The filter follows the eased level, once a buffer (a few ms).
        const float hz = std::min(DarkHz + (BrightHz - DarkHz) * gain_, 0.45f * rate);
        const float a = std::exp(-6.2831853f * hz / static_cast<float>(rate));
        if (is_float) {
            Run(static_cast<float*>(buffer), frames, channels, target, volume, left, right, step,
                attack, release, a);
        } else {
            Run(static_cast<std::int16_t*>(buffer), frames, channels, target, volume, left, right,
                step, attack, release, a);
        }
    }
    float Level() const { return gain_; }

private:
    float Read() {
        const std::size_t count = pcm_.size() / 2;
        const auto i = static_cast<std::size_t>(pos_) % count;
        const std::size_t j = i + 1 < count ? i + 1 : 0;
        const float t = static_cast<float>(pos_ - std::floor(pos_));
        const float s = Sample(i) + (Sample(j) - Sample(i)) * t;
        return s;
    }
    float Sample(std::size_t i) const {
        const int bits = static_cast<unsigned char>(pcm_[2 * i]) |
                         (static_cast<unsigned char>(pcm_[2 * i + 1]) << 8);
        return static_cast<float>(bits >= 32768 ? bits - 65536 : bits) / 32768.0f;
    }
    template <class T> static void Add(T& d, float v) {
        if constexpr (std::is_same_v<T, float>) {
            d = std::clamp(d + v, -1.0f, 1.0f);
        } else {
            d = static_cast<T>(std::clamp(std::lround(d + v * 32768.0f), -32768L, 32767L));
        }
    }
    template <class T>
    void Run(T* out, std::size_t frames, unsigned channels, float target, float volume, float left,
             float right, double step, float attack, float release, float a) {
        const double count = static_cast<double>(pcm_.size() / 2);
        for (std::size_t i = 0; i < frames; ++i) {
            gain_ += (target - gain_) * (target > gain_ ? attack : release);
            if (target == 0.0f && gain_ < 0.003f) { // (-50 dB: off)
                gain_ = 0.0f;
                lp_ = 0.0f;
                return;
            }
            lp_ = (1.0f - a) * Read() + a * lp_;
            pos_ += step;
            if (pos_ >= count) {
                pos_ = std::fmod(pos_, count);
            }
            const float v = lp_ * gain_ * Gain * volume;
            Add(out[i * channels], v * (channels == 1 ? 1.0f : left));
            if (channels > 1) {
                Add(out[i * channels + 1], v * right);
            }
        }
    }

    std::span<const char> pcm_;
    double pos_{0.0};
    float gain_{0.0f};
    float lp_{0.0f};
    std::uint32_t seed_{0x2545F491u};
};

} // namespace VR::Grind
