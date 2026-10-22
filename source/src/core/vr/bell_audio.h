// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include "core/vr/seqlock.h"

namespace VR::Quick {
struct BellAudioFrame { std::int64_t ns{}; float strength{}; };
inline SeqLock<BellAudioFrame>& BellAudioChannel() {
    static SeqLock<BellAudioFrame> channel;
    return channel;
}
// One presenter writer, one main-port mixer; no game RNG, assets or playback allocations.
inline void PublishBellAudio(bool toll, std::int64_t now) {
    BellAudioChannel().Store({now, toll ? 1.0f : 0.65f});
}
inline BellAudioFrame LatestBellAudio() {
    BellAudioFrame frame{};
    BellAudioChannel().Load(frame, 4);
    return frame;
}
class BellAudioMixer {
public:
    void Mix(void* buffer, std::size_t buffer_bytes, std::size_t frames, unsigned channels, unsigned rate, bool floating,
             BellAudioFrame event, std::int64_t now) {
        if (!buffer || !frames || rate < 8000 ||
            (channels != 1 && channels != 2 && channels != 8)) return;
        const auto sample_bytes = floating ? sizeof(float) : sizeof(std::int16_t);
        if (frames > buffer_bytes / (channels * sample_bytes) ||
            reinterpret_cast<std::uintptr_t>(buffer) % sample_bytes != 0) return;
        if (event.ns > seen_) {
            seen_ = event.ns;
            if (event.ns > 0 && now >= event.ns && now-event.ns < 150'000'000 &&
                std::isfinite(event.strength) && event.strength > 0) {
                // Restart a bounded voice rather than accumulating overlapping shakes.
                age_ = 0;
                gain_ = 0.07f * std::clamp(event.strength, 0.0f, 1.0f);
            }
        }
        if (age_ >= 0.8) return;
        if (floating) Samples(static_cast<float*>(buffer),frames,channels,rate);
        else Samples(static_cast<std::int16_t*>(buffer),frames,channels,rate);
    }
private:
    template<class T> static void Add(T& destination, double voice) {
        if constexpr (std::is_same_v<T,float>)
            destination=std::clamp(destination+static_cast<float>(voice),-1.0f,1.0f);
        else destination=static_cast<T>(std::clamp(std::lround(destination+voice*32768),-32768L,32767L));
    }
    template<class T> void Samples(T* out,std::size_t frames,unsigned channels,unsigned rate) {
        const double dt=1.0/rate;
        // Rebuild oscillators once per buffer to bound numerical drift. No transcendental
        // functions inside the sample loop; fixed storage and at most one 0.8-second voice.
        constexpr double frequencies[]{1240,1977,2891};
        constexpr double decay[]{7,12,20};
        constexpr double weights[]{1,0.38,0.18};
        double sine[3], cosine[3], step_sine[3], step_cosine[3], envelope[3], fall[3];
        for (int j=0;j<3;++j) {
            const double omega=6.283185307179586*frequencies[j];
            sine[j]=std::sin(omega*age_); cosine[j]=std::cos(omega*age_);
            step_sine[j]=std::sin(omega*dt); step_cosine[j]=std::cos(omega*dt);
            envelope[j]=weights[j]*std::exp(-age_*decay[j]);
            fall[j]=std::exp(-dt*decay[j]);
        }
        for (std::size_t i=0;i<frames && age_<0.8;++i,age_+=dt) {
            const double attack=std::min(age_/0.003,1.0);
            const double tail=std::clamp((0.8-age_)/0.02,0.0,1.0);
            double sum=0;
            for (int j=0;j<3;++j) {
                sum+=sine[j]*envelope[j];
                const double next=sine[j]*step_cosine[j]+cosine[j]*step_sine[j];
                cosine[j]=cosine[j]*step_cosine[j]-sine[j]*step_sine[j];
                sine[j]=next; envelope[j]*=fall[j];
            }
            const double voice=gain_*attack*tail*sum;
            Add(out[i*channels],voice*(channels==1?1.0:0.8));
            if(channels>1) Add(out[i*channels+1],voice*0.6);
        }
    }
    std::int64_t seen_{};
    double age_{1.0};
    float gain_{};
};
}
