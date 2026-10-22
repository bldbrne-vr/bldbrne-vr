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

namespace VR::Whip {

// The Beast Cutter's sounds (chosen 2026-10-14, the reel audition): while the chain
// reels in, its old closing clink (beast_chain_clink.pcm, "close_old") back to back; once it is
// fully in, "close_noko2_ring": the game's whip-to-blade lock (w_change_whip_noko2, its 0.245 s
// lead-in cut) with a sword's ring under it (sword-x-sword1 at 0.45), both from the player's own
// sprj_main_eng.fsb (core/vr/game_sounds.h); until it decodes, or if it cannot, the old clink.
constexpr const char* CutterLockSample = "w_change_whip_noko2";
constexpr const char* CutterRingSample = "sword-x-sword1";
constexpr double CutterLockLeadS = 0.245;
constexpr float CutterRingMix = 0.45f;
constexpr float CutterReelGain = 0.48f;  // close_old looped, at the old clink's own level
constexpr float CutterCloseGain = 0.39f; // the close's peak: 1.5x the old clink's (the audition)

// 24 kHz mono S16LE in and out: the lock (from its lead-in, a 2 ms fade in) and the ring, each to
// full scale, mixed, the sum to full scale. Empty if the lock is missing; the ring may be.
inline std::vector<char> MakeCutterClose(const std::vector<char>& lock,
                                         const std::vector<char>& ring) {
    const auto read = [](const std::vector<char>& p) {
        std::vector<float> v(p.size() / 2);
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>(
                static_cast<unsigned char>(p[2 * i]) |
                (static_cast<unsigned char>(p[2 * i + 1]) << 8))));
        }
        return v;
    };
    const auto normalize = [](std::vector<float>& v) {
        float peak = 0.0f;
        for (const float x : v) {
            peak = std::max(peak, std::abs(x));
        }
        if (peak > 0.0f) {
            for (float& x : v) {
                x /= peak;
            }
        }
    };
    std::vector<float> a = read(lock);
    const auto lead = static_cast<std::size_t>(CutterLockLeadS * 24000.0);
    if (a.size() <= lead + 240) {
        return {};
    }
    a.erase(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(lead));
    for (std::size_t i = 0; i < 48 && i < a.size(); ++i) {
        a[i] *= static_cast<float>(i) / 48.0f;
    }
    normalize(a);
    std::vector<float> b = read(ring);
    normalize(b);
    a.resize(std::max(a.size(), b.size()), 0.0f);
    for (std::size_t i = 0; i < b.size(); ++i) {
        a[i] += b[i] * CutterRingMix;
    }
    normalize(a);
    std::vector<char> out(a.size() * 2);
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto q = static_cast<std::int16_t>(std::clamp(std::lround(a[i] * 32767.0f),
                                                            -32768L, 32767L));
        out[2 * i] = static_cast<char>(q & 0xFF);
        out[2 * i + 1] = static_cast<char>((q >> 8) & 0xFF);
    }
    return out;
}

struct RecallSound {
    float drag{0.0f};
    bool clink{false};
};

// Game-thread gate: use the actual shortening chain, not just the recall request.
// Reversing into another swing cancels the scrape without manufacturing a closing clink.
// The clink waits for the blade, not the length: unlocked links still swing into the closed
// shape 80-110 ms after the length closes. `deviation` (m): the chain's farthest point from
// its closed shape (Chain::ClosedDeviation); a blade that never settles clinks at the timeout.
// The drag runs on (at its last level) while the links settle, straight into the clink: no
// silent gap between the chain coming in and its closing.
class RecallSoundGate {
public:
    static constexpr float SettledM = 0.02f;
    static constexpr float SettleTimeoutS = 0.15f;

    RecallSound Step(bool enabled, bool returning, float extension, float deviation, float dt) {
        RecallSound out{};
        if (!enabled || !std::isfinite(extension) || !std::isfinite(dt) || dt < 0 || dt > 0.1f) {
            Reset();
            return out;
        }
        if (dt == 0) return out;
        if (!have_previous_) {
            previous_ = extension;
            have_previous_ = true;
        }
        const bool closed = extension <= 1.001f;
        // A new swing before the blade settled: no clink for this reel-in.
        if (waiting_ && (!returning || !closed)) {
            waiting_ = false;
            armed_ = false;
            drag_ = 0.0f;
        }
        if (extension > 1.08f) armed_ = true;
        if (returning && armed_) {
            const float rate = (previous_ - extension) / dt;
            if (closed) {
                settle_ = waiting_ ? settle_ + dt : 0.0f;
                waiting_ = true;
                if (deviation <= SettledM || settle_ >= SettleTimeoutS) {
                    out.clink = true;
                    armed_ = waiting_ = false;
                    drag_ = 0.0f;
                } else {
                    out.drag = drag_;
                }
            } else if (rate > 0.02f) {
                out.drag = drag_ = std::clamp(rate / 10.0f, 0.25f, 1.0f);
            }
        }
        previous_ = extension;
        return out;
    }
    void Reset() { *this = {}; }
private:
    float previous_{1.0f};
    float settle_{0.0f}; // s since the length closed
    float drag_{0.0f};   // the drag's last level while the chain came in
    bool have_previous_{false};
    bool armed_{false};
    bool waiting_{false}; // closed, the clink waits for the blade
};

struct RecallAudioFrame {
    std::int64_t ns{0};
    std::int64_t clink_ns{0};
    std::uint64_t clink_id{0};
    float drag{0.0f};
    float pan{0.0f};
    bool enabled{false};
};

// Single game-thread writer; the first main audio port is the sole playback owner.
void PublishRecallAudio(RecallSound sound, bool enabled, float pan, std::int64_t now);
RecallAudioFrame LatestRecallAudio();
std::span<const char> RecallClinkPcm();

// Allocation-free playback of embedded 24 kHz mono S16LE excerpts. Mixed before the
// existing backend's volume/mute and routing, for both SDL and OpenAL/HRTF.
class RecallAudioMixer {
public:
    // `clink_gain`: the one-shot's level (the whip's clink 0.48; the gun's reload and empty click
    // their own, core/vr/gun_reload_audio.h); `drag_gain`: the loop's at full speed.
    RecallAudioMixer(std::span<const char> drag, std::span<const char> clink,
                     float clink_gain = 0.48f, float drag_gain = 0.36f)
        : drag_(drag), clink_(clink), clink_gain_(clink_gain), drag_level_(drag_gain) {}

    // Another one-shot (the Beast Cutter's close once it decodes); never under one playing.
    bool SetClink(std::span<const char> clink, float gain) {
        if (clink_live_ || clink.size() < 4) {
            return false;
        }
        clink_ = clink;
        clink_gain_ = gain;
        return true;
    }

    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             RecallAudioFrame state, std::int64_t now) {
        if (!buffer || !frames || !rate || (channels != 1 && channels != 2 && channels != 8)) return;
        const bool fresh = state.ns > 0 && now >= state.ns && now - state.ns < 150'000'000;
        const bool enabled = fresh && state.enabled;
        const float target = enabled && std::isfinite(state.drag) ? std::clamp(state.drag, 0.0f, 1.0f) : 0.0f;
        if (state.clink_id != clink_id_) {
            clink_id_ = state.clink_id;
            if (enabled && state.clink_ns > 0 && now >= state.clink_ns && now - state.clink_ns < 150'000'000) {
                clink_pos_ = 0.0;
                clink_live_ = true;
            }
        }
        if (!enabled) clink_live_ = false;
        if (target > 0 && !drag_requested_ && drag_gain_ < 0.001f) drag_pos_ = 0.0;
        drag_requested_ = target > 0;
        if (target == 0 && drag_gain_ == 0 && !clink_live_) return;
        const float pan = std::isfinite(state.pan) ? std::clamp(state.pan, -1.0f, 1.0f) : 0.0f;
        // Constant-power stereo pan; front L/R indices are common to both console layouts.
        const float left = std::sqrt(0.5f * (1.0f - pan));
        const float right = std::sqrt(0.5f * (1.0f + pan));
        const double step = 24000.0 / rate;
        const float attack = 1.0f - std::exp(-1.0f / (0.005f * rate));
        const float release = 1.0f / (0.012f * rate);
        if (is_float) MixSamples(static_cast<float*>(buffer), frames, channels, target, left, right, step, attack, release);
        else MixSamples(static_cast<std::int16_t*>(buffer), frames, channels, target, left, right, step, attack, release);
    }
private:
    static float Sample(std::span<const char> pcm, std::size_t index) {
        const auto lo = static_cast<unsigned char>(pcm[index * 2]);
        const auto hi = static_cast<unsigned char>(pcm[index * 2 + 1]);
        const int bits = lo | (hi << 8);
        return static_cast<float>(bits >= 32768 ? bits - 65536 : bits) / 32768.0f;
    }
    static float Read(std::span<const char> pcm, double& position, double step, bool loop) {
        const std::size_t count = pcm.size() / 2;
        if (count < 2 || position >= count) return 0;
        const auto i = static_cast<std::size_t>(position);
        const auto j = i + 1 < count ? i + 1 : loop ? 0 : i;
        const float sample = Sample(pcm, i) + (Sample(pcm, j) - Sample(pcm, i)) * static_cast<float>(position - i);
        position += step;
        if (loop && position >= count) position = std::fmod(position, static_cast<double>(count));
        return sample;
    }
    template<class T> static void Add(T& destination, float voice) {
        if (voice == 0) return;
        if constexpr (std::is_same_v<T, float>) destination = std::clamp(destination + voice, -1.0f, 1.0f);
        else destination = static_cast<T>(std::clamp(std::lround(destination + voice * 32768.0f), -32768L, 32767L));
    }
    template<class T> void MixSamples(T* output, std::size_t frames, unsigned channels,
                                     float target, float left, float right, double step,
                                     float attack, float release) {
        for (std::size_t i = 0; i < frames; ++i) {
            drag_gain_ = target > drag_gain_ ? drag_gain_ + (target - drag_gain_) * attack
                                              : std::max(target, drag_gain_ - release);
            if (target == 0 && drag_gain_ < 0.0001f) drag_gain_ = 0;
            float voice = 0;
            if (drag_gain_ > 0) voice += Read(drag_, drag_pos_, step, true) * (drag_level_ * drag_gain_);
            if (clink_live_) {
                voice += Read(clink_, clink_pos_, step, false) * clink_gain_;
                if (clink_pos_ >= clink_.size() / 2) clink_live_ = false;
            }
            Add(output[i * channels], voice * (channels == 1 ? 1.0f : left));
            if (channels > 1) Add(output[i * channels + 1], voice * right);
        }
    }
    std::span<const char> drag_, clink_;
    float clink_gain_{0.48f};
    float drag_level_{0.36f};
    double drag_pos_{0}, clink_pos_{0};
    float drag_gain_{0};
    std::uint64_t clink_id_{0};
    bool clink_live_{false}, drag_requested_{false};
};
} // namespace VR::Whip
