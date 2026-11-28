// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/libraries/audio/openal_manager.h"

// Fallbacks in case the alext.h in use predates these extensions.
#ifndef ALC_SOFT_output_mode
#define ALC_OUTPUT_MODE_SOFT 0x19AC
#define ALC_MONO_SOFT 0x1500
#define ALC_STEREO_SOFT 0x1501
#define ALC_STEREO_BASIC_SOFT 0x19AE
#define ALC_STEREO_UHJ_SOFT 0x19AF
#define ALC_STEREO_HRTF_SOFT 0x19B2
#define ALC_SURROUND_5_1_SOFT 0x1504
#define ALC_SURROUND_6_1_SOFT 0x1505
#define ALC_SURROUND_7_1_SOFT 0x1506
#endif
#ifndef AL_SOFT_direct_channels_remix
#define AL_REMIX_UNMATCHED_SOFT 0x0002
#endif
#include <chrono>
#include <queue>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audio_clock.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/kernel/threads.h"
#include "core/vr/audio_settings.h"
#include "core/vr/mirror_world.h"
#include "core/vr/vr_session.h"

// SIMD support detection
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_SSE2
#endif

namespace Libraries::AudioOut {

// Volume constants
constexpr float VOLUME_0DB = 32768.0f; // 1 << 15
constexpr float INV_VOLUME_0DB = 1.0f / VOLUME_0DB;
constexpr float VOLUME_EPSILON = 0.001f;
// Timing constants
constexpr u64 VOLUME_CHECK_INTERVAL_US = 50000; // Check every 50ms
constexpr u64 MIN_SLEEP_THRESHOLD_US = 10;
constexpr u64 TIMING_RESYNC_THRESHOLD_US = 100000; // Resync if >100ms behind

// OpenAL constants
constexpr ALsizei NUM_BUFFERS = 6;
constexpr ALsizei BUFFER_QUEUE_THRESHOLD = 2; // Queue more buffers when below this

// Channel positions
enum ChannelPos : u8 {
    FL = 0,
    FR = 1,
    FC = 2,
    LF = 3,
    SL = 4,
    SR = 5,
    BL = 6,
    BR = 7,
    STD_SL = 6,
    STD_SR = 7,
    STD_BL = 4,
    STD_BR = 5
};

class OpenALPortBackend : public PortBackend {
public:
    explicit OpenALPortBackend(const PortOut& port)
        : frame_size(port.format_info.FrameSize()), guest_buffer_size(port.BufferSize()),
          buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          num_channels(port.format_info.num_channels), is_float(port.format_info.is_float),
          is_std(port.format_info.is_std), channel_layout(port.format_info.channel_layout),
          device_registered(false), device_name(GetDeviceName(port.type)) {

        if (!Initialize(port.type)) {
            LOG_ERROR(Lib_AudioOut, "Failed to initialize OpenAL audio backend");
        }
    }

    ~OpenALPortBackend() override {
        Cleanup();
        if (device_registered) {
            OpenALDevice::GetInstance().UnregisterPort(device_name);
        }
    }

    void Output(void* ptr) override {
        if (!source || !convert) [[unlikely]] {
            return;
        }
        if (ptr == nullptr) [[unlikely]] {
            return;
        }
        u32 generation = 0;
        const auto context = device_context->Bind(device_name, &generation);
        if (!context) {
            pace_by_period = true;
            return;
        }
        if (lead_buffers > 0) {
            ALCint connected = ALC_TRUE;
            alcGetIntegerv(alcGetContextsDevice(alcGetCurrentContext()), ALC_CONNECTED, 1, &connected);
            if (!connected) {
                pace_by_period = true;
                return;
            }
        }
        if (generation != seen_reopen) [[unlikely]] {
            seen_reopen = generation;
            FlushQueue();
        }

        UpdateVolumeIfChanged();
        const u64 current_time = HostTimeUs();

        // Convert audio data ONCE per call
        if (use_native_float) {
            convert(ptr, al_buffer_float.data(), buffer_frames, nullptr);
        } else {
            convert(ptr, al_buffer_s16.data(), buffer_frames, nullptr);
        }

        if (fold_lfe) {
            FoldLfeIntoFronts();
        }
        if (mirror_sides) [[unlikely]] {
            // Left-handed play (core/vr/mirror_world.h): left and right trade, as the world does.
            const u32 out_channels = downmix_to_stereo ? 2u : num_channels;
            if (use_native_float) {
                VR::Mirror::SwapSides(std::span{al_buffer_float}, out_channels);
            } else {
                VR::Mirror::SwapSides(std::span{al_buffer_s16}, out_channels);
            }
        }

        // A stopped streaming source starts again at the head of its old queue.
        // Reclaim it before adding fresh PCM, for both timing modes.
        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        ReclaimProcessed();
        if (state == AL_STOPPED) [[unlikely]] {
            alSourceRewind(source);
            ++ran_dry;
        }

        if (!available_buffers.empty()) {
            if (!QueueConvertedBuffer()) {
                pace_by_period = true;
                return;
            }
        } else {
            ++dropped_full; // the queue is full: this buffer is lost (a click)
        }

        // If the mixer stopped during submission, recover on the next call.
        ALint queued = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);

        if (lead_buffers > 0) {
            // Rewound after running dry (or a flushed queue): plays again once the lead is back,
            // one gap instead of a run of them.
            if (state == AL_INITIAL && queued >= static_cast<ALint>(lead_buffers)) {
                alSourcePlay(source);
                alGetSourcei(source, AL_SOURCE_STATE, &state);
                pace_by_period = state != AL_PLAYING;
            } else if (state == AL_PLAYING) {
                pace_by_period = false;
            }
        } else if (state == AL_INITIAL && queued > 0) {
            LOG_DEBUG(Lib_AudioOut, "Audio underrun detected (queued: {}), restarting source",
                      queued);
            alSourcePlay(source);
        }
        // Every 10 s, only when something went wrong: what the listener would hear as clicks.
        if (current_time - glitch_log_us >= 10'000'000) {
            if (dropped_full || ran_dry) {
                LOG_INFO(Lib_AudioOut,
                         "Audio port ({} frames, {} ch): last {:.0f} s, {} buffers dropped (queue "
                         "full), {} restarts (ran dry), {} queued now",
                         buffer_frames, num_channels,
                         static_cast<double>(current_time - glitch_log_us) * 1e-6, dropped_full,
                         ran_dry, queued);
            }
            dropped_full = ran_dry = 0;
            glitch_log_us = current_time;
        }

        if (lead_buffers > 0) {
            // Hold(), called by the port's thread once it lets go of the port's lock.
        } else if (queued >= 2) { // Only sleep if we have healthy buffer queue
            HandleTiming(current_time);
        } else {
            next_output_time = current_time + period_us;
        }

        last_output_time.store(Kernel::sceKernelGetProcessTime(), std::memory_order_release);
        output_count++;
    }

    bool PacesItself() const override {
        return lead_buffers > 0 && source != 0 && convert != nullptr;
    }

    void Hold() override {
        if (!pace_by_period) {
            if (const auto context = device_context->Bind(device_name)) {
                HoldLead();
                return;
            }
        }
        // No working playback clock. Throttle each submitted buffer outside the port lock,
        // including retries until a disconnected or failed source actually plays again.
        Common::PreciseSleepUntil(std::chrono::steady_clock::now() +
                                      std::chrono::microseconds(period_us),
                                  std::chrono::nanoseconds{0});
    }
    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        if (!source) [[unlikely]] {
            return;
        }
        const auto context = device_context->Bind(device_name);
        if (!context) {
            return;
        }

        float max_channel_gain = 0.0f;
        const u32 channels_to_check = std::min(num_channels, 8u);

        for (u32 i = 0; i < channels_to_check; i++) {
            const float channel_gain = static_cast<float>(ch_volumes[i]) * INV_VOLUME_0DB;
            max_channel_gain = std::max(max_channel_gain, channel_gain);
        }
        game_gain.store(max_channel_gain, std::memory_order_release);

        const float slider_gain = EmulatorSettings.GetVolumeSlider() * 0.01f;
        const float total_gain = max_channel_gain * slider_gain;

        const float current = current_gain.load(std::memory_order_acquire);
        if (std::abs(total_gain - current) < VOLUME_EPSILON) {
            return;
        }

        alSourcef(source, AL_GAIN, total_gain);

        ALenum error = alGetError();
        if (error == AL_NO_ERROR) {
            current_gain.store(total_gain, std::memory_order_release);
            LOG_DEBUG(Lib_AudioOut,
                      "Set combined audio gain to {:.3f} (channel: {:.3f}, slider: {:.3f})",
                      total_gain, max_channel_gain, slider_gain);
        } else {
            LOG_ERROR(Lib_AudioOut, "Failed to set OpenAL source gain: {}",
                      GetALErrorString(error));
        }
    }

    u64 GetLastOutputTime() const {
        return last_output_time.load(std::memory_order_acquire);
    }

private:
    bool Initialize(OrbisAudioOutPort type) {
        // Register this port with the device manager
        if (!OpenALDevice::GetInstance().RegisterPort(device_name)) {
            if (device_name == "None") {
                LOG_INFO(Lib_AudioOut, "Audio device disabled for port type {}",
                         static_cast<int>(type));
            } else {
                LOG_ERROR(Lib_AudioOut, "Failed to register OpenAL device '{}'", device_name);
            }
            return false;
        }

        device_registered = true;
        device_context = &OpenALDevice::GetInstance();

        // Make this device's context current
        const auto context = device_context->Bind(device_name, &seen_reopen);
        if (!context) {
            LOG_ERROR(Lib_AudioOut, "Failed to make OpenAL context current for device '{}'",
                      device_name);
            return false;
        }

        // Log device info
        LOG_INFO(Lib_AudioOut, "Using OpenAL device for port type {}: '{}'", static_cast<int>(type),
                 device_name);

        // Calculate timing parameters
        period_us = (1000000ULL * buffer_frames + sample_rate / 2) / sample_rate;
        // audio_lead_ms (core/vr/audio_settings.h): that much of the game's sound kept queued,
        // and room for twice it (OpenAL takes ~20 ms at a time); 0: the old 6 buffers.
        if (const int lead_ms = VR::Sound::LeadMs(); lead_ms > 0 && period_us > 0) {
            lead_buffers = static_cast<u32>(std::clamp<u64>(
                (static_cast<u64>(lead_ms) * 1000 + period_us - 1) / period_us, 2, 64));
            pool_buffers = lead_buffers * 2 + 4;
        }
        if (lead_buffers > 0) {
            LOG_INFO(Lib_AudioOut,
                     "Audio port timing: {} ms queued ahead ({} buffers of {} frames, room for "
                     "{}), the output at the highest priority (audio_lead_ms={})",
                     (lead_buffers * period_us + 500) / 1000, lead_buffers, buffer_frames,
                     pool_buffers, VR::Sound::LeadMs());
        } else {
            LOG_INFO(Lib_AudioOut,
                     "Audio port timing: the old {}-buffer queue on a {} us clock "
                     "(audio_lead_ms=0)",
                     pool_buffers, period_us);
        }

        // Check for AL_EXT_FLOAT32 extension
        has_float_ext = alIsExtensionPresent("AL_EXT_FLOAT32");
        if (has_float_ext && is_float) {
            LOG_INFO(Lib_AudioOut, "AL_EXT_FLOAT32 extension detected - using native float format");
        }

        // Determine OpenAL format
        if (!DetermineOpenALFormat()) {
            LOG_ERROR(Lib_AudioOut, "Unsupported audio format for OpenAL");
            return false;
        }

        ALCdevice* alc_dev = alcGetContextsDevice(alcGetCurrentContext());
        ALCint output_mode = 0;
        bool know_output_mode = false;
        if (alc_dev && alcIsExtensionPresent(alc_dev, "ALC_SOFT_output_mode")) {
            alcGetIntegerv(alc_dev, ALC_OUTPUT_MODE_SOFT, 1, &output_mode);
            know_output_mode = true;
        }
        const bool output_has_lfe = output_mode == ALC_SURROUND_5_1_SOFT ||
                                    output_mode == ALC_SURROUND_6_1_SOFT ||
                                    output_mode == ALC_SURROUND_7_1_SOFT;
        fold_lfe = know_output_mode && !output_has_lfe && num_channels >= 6 && !downmix_to_stereo;
        if (fold_lfe) {
            LOG_INFO(Lib_AudioOut, "Output has no LFE channel; folding buffer LFE into fronts");
        }
        if (alc_dev && alcIsExtensionPresent(alc_dev, "ALC_SOFT_HRTF")) {
            ALCint hrtf_on = 0;
            alcGetIntegerv(alc_dev, ALC_HRTF_SOFT, 1, &hrtf_on);
            hrtf_active = hrtf_on == ALC_TRUE;
        }

        const bool stereo_output = output_mode == ALC_MONO_SOFT || output_mode == ALC_STEREO_SOFT ||
                                   output_mode == ALC_STEREO_BASIC_SOFT ||
                                   output_mode == ALC_STEREO_UHJ_SOFT ||
                                   output_mode == ALC_STEREO_HRTF_SOFT;
        if (know_output_mode && stereo_output && !hrtf_active && num_channels >= 6 &&
            !downmix_to_stereo) {
            downmix_to_stereo = true;
            use_native_float = has_float_ext;
            format = has_float_ext ? AL_FORMAT_STEREO_FLOAT32 : AL_FORMAT_STEREO16;
            fold_lfe = false; // The downmix converters already carry LFE.
            LOG_INFO(Lib_AudioOut, "Stereo output: using internal {}ch->stereo downmix ({})",
                     num_channels, has_float_ext ? "float" : "int16");
        }

        // Allocate buffers based on format
        const u32 out_channels = downmix_to_stereo ? 2u : num_channels;
        if (use_native_float) {
            al_buffer_float.resize(buffer_frames * out_channels);
            buffer_size_bytes = buffer_frames * out_channels * sizeof(float);
        } else {
            al_buffer_s16.resize(buffer_frames * out_channels);
            buffer_size_bytes = buffer_frames * out_channels * sizeof(s16);
        }

        // Select optimal converter function
        if (!SelectConverter()) {
            return false;
        }

        // Generate OpenAL source and buffers
        if (!CreateOpenALObjects()) {
            return false;
        }

        // Initialize current gain
        current_gain.store(EmulatorSettings.GetVolumeSlider() * 0.01f, std::memory_order_relaxed);
        alSourcef(source, AL_GAIN, current_gain.load(std::memory_order_relaxed));

        // Prime buffers with silence (with a lead: the lead, the rest is room)
        const size_t primed = lead_buffers > 0 ? lead_buffers : buffers.size() - 1;
        // The conversion buffer is still zero-filled. Keep even a failed priming
        // buffer available for the first real submission.
        size_t queued = 0;
        while (queued < primed && QueueConvertedBuffer()) {
            ++queued;
        }
        if (queued == primed) {
            alSourcePlay(source);
        } else {
            pace_by_period = true;
        }

        LOG_INFO(Lib_AudioOut,
                 "Initialized OpenAL backend ({} Hz, {} ch, {} format, {}) for device '{}'",
                 sample_rate, num_channels, is_float ? "float" : "int16",
                 use_native_float ? "native" : "converted", device_name);
        return true;
    }

    void Cleanup() {
        if (!device_context) {
            return;
        }
        const auto context = device_context->Bind(device_name);
        if (!context) {
            return;
        }

        if (source) {
            // Deleting the source releases its queue, including any unplayed priming buffers.
            alDeleteSources(1, &source);
            source = 0;
        }

        if (!buffers.empty()) {
            alDeleteBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
            buffers.clear();
        }
    }

    std::string GetDeviceName(OrbisAudioOutPort type) const {
        switch (type) {
        case OrbisAudioOutPort::Main:
        case OrbisAudioOutPort::Bgm:
            return EmulatorSettings.GetOpenALMainOutputDevice();
        case OrbisAudioOutPort::PadSpk:
            return EmulatorSettings.GetOpenALPadSpkOutputDevice();
        default:
            return EmulatorSettings.GetOpenALMainOutputDevice();
        }
    }

    void UpdateVolumeIfChanged() {
        const u64 current_time = HostTimeUs();

        if (current_time - last_volume_check_time < VOLUME_CHECK_INTERVAL_US) {
            return;
        }

        last_volume_check_time = current_time;

        const float config_volume =
            EmulatorSettings.GetVolumeSlider() * 0.01f * game_gain.load(std::memory_order_acquire);
        const float stored_gain = current_gain.load(std::memory_order_acquire);

        if (std::abs(config_volume - stored_gain) > VOLUME_EPSILON) {
            alSourcef(source, AL_GAIN, config_volume);

            ALenum error = alGetError();
            if (error == AL_NO_ERROR) {
                current_gain.store(config_volume, std::memory_order_release);
                LOG_DEBUG(Lib_AudioOut, "Updated audio gain to {:.3f}", config_volume);
            } else {
                LOG_ERROR(Lib_AudioOut, "Failed to set audio gain: {}", GetALErrorString(error));
            }
        }
    }

    // A buffer belongs to the free list until OpenAL accepts it. An upload failure
    // must never enqueue the previous contents of that buffer.
    bool QueueConvertedBuffer() {
        const ALuint buffer_id = available_buffers.back();
        const void* samples = use_native_float ? static_cast<const void*>(al_buffer_float.data())
                                               : static_cast<const void*>(al_buffer_s16.data());
        alGetError(); // isolate these operations from earlier errors on this context
        alBufferData(buffer_id, format, samples, buffer_size_bytes, sample_rate);
        ALenum error = alGetError();
        if (error == AL_NO_ERROR) {
            alSourceQueueBuffers(source, 1, &buffer_id);
            error = alGetError();
        }
        if (error != AL_NO_ERROR) {
            if (!submission_failed) {
                LOG_WARNING(Lib_AudioOut, "OpenAL buffer submission failed: {}", GetALErrorString(error));
            }
            submission_failed = true;
            return false;
        }
        submission_failed = false;
        available_buffers.pop_back();
        return true;
    }

    // After the device moved to another output (OpenALDevice::FollowOutputLocked): the queue
    // filled up while the device was stopped and would stay full, every timing wobble then
    // dropping a buffer (clicks). Start it over: stop, take every buffer back, re-time.
    void FlushQueue() {
        alSourceStop(source);
        // Detach also clears unplayed buffers when the source was still priming.
        alGetError();
        alSourcei(source, AL_BUFFER, 0);
        if (alGetError() == AL_NO_ERROR) {
            available_buffers = buffers;
        } else {
            ReclaimProcessed();
        }
        alSourceRewind(source);
        next_output_time = 0;
        LOG_INFO(Lib_AudioOut, "Audio port queue restarted after an output change");
    }

    // Every buffer the device has played back into the pool.
    void ReclaimProcessed() {
        ALint processed = 0;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0) {
            alGetError(); // a stale error must not hide a successful unqueue
        }
        while (processed-- > 0) {
            ALuint buffer_id;
            alSourceUnqueueBuffers(source, 1, &buffer_id);
            if (alGetError() != AL_NO_ERROR) {
                break;
            }
            available_buffers.push_back(buffer_id);
        }
    }

    // audio_lead_ms: wait while the lead is queued. The game's thread waits with this (its next
    // buffer is taken once it returns), so the game's sound runs at the device's own rate, the
    // queue never drifting full or empty; below the lead (filling, or rewound after running dry)
    // it returns at once and the next buffer comes straight away. OpenAL takes ~20 ms at a time,
    // so a wait is up to that long; MaxHold bounds it should the device stop taking any. It looks
    // every half buffer (~2.7 ms): OpenAL's next take is ~20 ms off once it took some, so a
    // finer look only adds wake-ups at the highest priority.
    void HoldLead() {
        const auto poll = std::chrono::microseconds(std::max<u64>(period_us / 2, 1000));
        constexpr auto MaxHold = std::chrono::microseconds(50'000);
        const auto give_up = std::chrono::steady_clock::now() + MaxHold;
        while (true) {
            ALint state = 0, queued = 0, processed = 0;
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                return; // building the lead: the next buffer now
            }
            alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
            alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
            if (queued - processed < static_cast<ALint>(lead_buffers)) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= give_up) {
                return;
            }
            Common::PreciseSleepUntil(std::min(now + poll, give_up), std::chrono::nanoseconds{0});
        }
    }

    void HandleTiming(u64 current_time) {
        if (next_output_time == 0) [[unlikely]] {
            next_output_time = current_time + period_us;
            return;
        }

        const s64 time_diff = static_cast<s64>(current_time - next_output_time);

        if (time_diff > static_cast<s64>(TIMING_RESYNC_THRESHOLD_US)) [[unlikely]] {
            next_output_time = current_time + period_us;
        } else if (time_diff < 0) {
            const u64 time_to_wait = static_cast<u64>(-time_diff);
            next_output_time += period_us;

            if (time_to_wait > MIN_SLEEP_THRESHOLD_US) {
                const u64 sleep_duration = time_to_wait - MIN_SLEEP_THRESHOLD_US;
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_duration));
            }
        } else {
            next_output_time += period_us;
        }
    }

    bool DetermineOpenALFormat() {
        alGetError();

        // Try to use native float formats if extension is available
        if (is_float && has_float_ext) {
            switch (num_channels) {
            case 1:
                format = AL_FORMAT_MONO_FLOAT32;
                use_native_float = true;
                return true;
            case 2:
                format = AL_FORMAT_STEREO_FLOAT32;
                use_native_float = true;
                return true;
            case 4:
                format = alGetEnumValue("AL_FORMAT_QUAD32");
                if (format != 0 && alGetError() == AL_NO_ERROR) {
                    use_native_float = true;
                    return true;
                }
                break;
            case 6:
                format = alGetEnumValue("AL_FORMAT_51CHN32");
                if (format != 0 && alGetError() == AL_NO_ERROR) {
                    use_native_float = true;
                    return true;
                }
                break;
            case 8:
                format = alGetEnumValue("AL_FORMAT_71CHN32");
                if (format != 0 && alGetError() == AL_NO_ERROR) {
                    use_native_float = true;
                    return true;
                }
                break;
            }

            LOG_WARNING(
                Lib_AudioOut,
                "Float format for {} channels not supported, falling back to S16 conversion",
                num_channels);
        }

        // Fall back to S16 formats (with conversion if needed)
        use_native_float = false;

        if (is_float) {
            // Will need to convert float to S16
            format = AL_FORMAT_MONO16;

            switch (num_channels) {
            case 1:
                format = AL_FORMAT_MONO16;
                break;
            case 2:
                format = AL_FORMAT_STEREO16;
                break;
            case 6:
                format = alGetEnumValue("AL_FORMAT_51CHN16");
                if (format == 0 || alGetError() != AL_NO_ERROR) {
                    LOG_WARNING(Lib_AudioOut, "5.1 format not supported, falling back to stereo");
                    format = AL_FORMAT_STEREO16;
                    downmix_to_stereo = true;
                }
                break;
            case 8:
                format = alGetEnumValue("AL_FORMAT_71CHN16");
                if (format == 0 || alGetError() != AL_NO_ERROR) {
                    LOG_WARNING(Lib_AudioOut, "7.1 format not supported, falling back to stereo");
                    format = AL_FORMAT_STEREO16;
                    downmix_to_stereo = true;
                }
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported float channel count: {}", num_channels);
                return false;
            }
        } else {
            // Native 16-bit integer formats
            switch (num_channels) {
            case 1:
                format = AL_FORMAT_MONO16;
                break;
            case 2:
                format = AL_FORMAT_STEREO16;
                break;
            case 6:
                format = alGetEnumValue("AL_FORMAT_51CHN16");
                if (format == 0 || alGetError() != AL_NO_ERROR) {
                    LOG_WARNING(Lib_AudioOut, "5.1 format not supported, falling back to stereo");
                    format = AL_FORMAT_STEREO16;
                    downmix_to_stereo = true;
                }
                break;
            case 8:
                format = alGetEnumValue("AL_FORMAT_71CHN16");
                if (format == 0 || alGetError() != AL_NO_ERROR) {
                    LOG_WARNING(Lib_AudioOut, "7.1 format not supported, falling back to stereo");
                    format = AL_FORMAT_STEREO16;
                    downmix_to_stereo = true;
                }
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported S16 channel count: {}", num_channels);
                return false;
            }
        }

        return true;
    }

    bool CreateOpenALObjects() {
        alGenSources(1, &source);
        if (alGetError() != AL_NO_ERROR) {
            LOG_ERROR(Lib_AudioOut, "Failed to generate OpenAL source");
            return false;
        }

        buffers.resize(pool_buffers);
        alGenBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
        if (alGetError() != AL_NO_ERROR) {
            LOG_ERROR(Lib_AudioOut, "Failed to generate OpenAL buffers");
            alDeleteSources(1, &source);
            source = 0;
            return false;
        }

        available_buffers = buffers;

        alSourcef(source, AL_PITCH, 1.0f);
        alSourcef(source, AL_GAIN, 1.0f);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        alSourcei(source, AL_LOOPING, AL_FALSE);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);

        if (alIsExtensionPresent("AL_SOFT_direct_channels")) {
            if (num_channels == 2 || downmix_to_stereo) {
                alSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
            } else if (num_channels > 2 && !hrtf_active &&
                       alIsExtensionPresent("AL_SOFT_direct_channels_remix")) {
                alSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_REMIX_UNMATCHED_SOFT);
            }
        }

        LOG_DEBUG(Lib_AudioOut, "Created OpenAL source {} with {} buffers", source, buffers.size());
        return true;
    }

    bool SelectConverter() {
        if (downmix_to_stereo) {
            if (use_native_float) {
                if (is_float) {
                    convert =
                        num_channels == 8 ? &DownmixF32_8CHToStereoF32 : &DownmixF32_6CHToStereoF32;
                } else {
                    convert =
                        num_channels == 8 ? &DownmixS16_8CHToStereoF32 : &DownmixS16_6CHToStereoF32;
                }
            } else if (is_float) {
                convert =
                    num_channels == 8 ? &DownmixF32_8CHToStereoS16 : &DownmixF32_6CHToStereoS16;
            } else {
                convert = num_channels == 8 ? &DownmixS16_8CHToStereo : &DownmixS16_6CHToStereo;
            }
            return true;
        }

        if (is_float && use_native_float) {
            // Native float - just copy/remap if needed
            switch (num_channels) {
            case 1:
                convert = &ConvertF32Mono;
                break;
            case 2:
                convert = &ConvertF32Stereo;
                break;
            case 8:
                convert = is_std ? &ConvertF32Std8CH : &ConvertF32_8CH;
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported float channel count: {}", num_channels);
                return false;
            }
        } else if (is_float && !use_native_float) {
            // Float to S16 conversion needed
            switch (num_channels) {
            case 1:
                convert = &ConvertF32ToS16Mono;
                break;
            case 2:
#ifdef HAS_SSE2
                convert = &ConvertF32ToS16StereoSIMD;
#else
                convert = &ConvertF32ToS16Stereo;
#endif
                break;
            case 8:
#ifdef HAS_SSE2
                convert = is_std ? &ConvertF32ToS16Std8CH : &ConvertF32ToS16_8CH_SIMD;
#else
                convert = is_std ? &ConvertF32ToS16Std8CH : &ConvertF32ToS16_8CH;
#endif
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported float channel count: {}", num_channels);
                return false;
            }
        } else {
            // S16 native - just copy
            switch (num_channels) {
            case 1:
                convert = &ConvertS16Mono;
                break;
            case 2:
                convert = &ConvertS16Stereo;
                break;
            case 8:
                convert = is_std ? &ConvertS16Std8CH : &ConvertS16_8CH;
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported S16 channel count: {}", num_channels);
                return false;
            }
        }

        return true;
    }

    const char* GetALErrorString(ALenum error) {
        switch (error) {
        case AL_NO_ERROR:
            return "AL_NO_ERROR";
        case AL_INVALID_NAME:
            return "AL_INVALID_NAME";
        case AL_INVALID_ENUM:
            return "AL_INVALID_ENUM";
        case AL_INVALID_VALUE:
            return "AL_INVALID_VALUE";
        case AL_INVALID_OPERATION:
            return "AL_INVALID_OPERATION";
        case AL_OUT_OF_MEMORY:
            return "AL_OUT_OF_MEMORY";
        default:
            return "Unknown AL error";
        }
    }

    // Converter function type
    using ConverterFunc = void (*)(const void* src, void* dst, u32 frames, const float* volumes);

    static inline s16 OrbisFloatToS16(float v) {
        if (std::abs(v) < 1.0e-20f)
            v = 0.0f;

        // Sony behavior: +1.0f -> 32767, -1.0f -> -32768
        const float scaled = v * 32768.0f;

        if (scaled >= 32767.0f)
            return 32767;
        if (scaled <= -32768.0f)
            return -32768;

        return static_cast<s16>(scaled + (scaled >= 0 ? 0.5f : -0.5f));
    }
    static void ConvertS16Mono(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);
        std::memcpy(d, s, frames * sizeof(s16));
    }

    static void ConvertS16Stereo(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);

        const u32 num_samples = frames << 1;
        std::memcpy(d, s, num_samples * sizeof(s16));
    }

    static void ConvertS16_8CH(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);

        const u32 num_samples = frames << 3;
        std::memcpy(d, s, num_samples * sizeof(s16));
    }
    static void ConvertS16Std8CH(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);

        for (u32 i = 0; i < frames; i++) {
            const u32 offset = i << 3;
            d[offset + FL] = s[offset + FL];
            d[offset + FR] = s[offset + FR];
            d[offset + FC] = s[offset + FC];
            d[offset + LF] = s[offset + LF];
            d[offset + SL] = s[offset + STD_SL];
            d[offset + SR] = s[offset + STD_SR];
            d[offset + BL] = s[offset + STD_BL];
            d[offset + BR] = s[offset + STD_BR];
        }
    }

    static inline s16 ClampSampleToS16(const float v) {
        return static_cast<s16>(std::clamp(v, -32768.0f, 32767.0f));
    }

    static constexpr float DOWNMIX_FRONT = 1.0f;
    static constexpr float DOWNMIX_CENTER = 0.7071f;
    static constexpr float DOWNMIX_SURROUND = 0.7071f;
    static constexpr float DOWNMIX_LFE = 0.0f; // console excludes LFE
    static void DownmixS16_6CHToStereo(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i * 6;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = ClampSampleToS16(DOWNMIX_FRONT * s[o + FL] + center + lfe +
                                            DOWNMIX_SURROUND * s[o + 4]);
            d[i * 2 + 1] = ClampSampleToS16(DOWNMIX_FRONT * s[o + FR] + center + lfe +
                                            DOWNMIX_SURROUND * s[o + 5]);
        }
    }
    static void DownmixS16_8CHToStereo(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        s16* d = static_cast<s16*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i << 3;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = ClampSampleToS16(DOWNMIX_FRONT * s[o + FL] + center + lfe +
                                            DOWNMIX_SURROUND * (s[o + 4] + s[o + 6]));
            d[i * 2 + 1] = ClampSampleToS16(DOWNMIX_FRONT * s[o + FR] + center + lfe +
                                            DOWNMIX_SURROUND * (s[o + 5] + s[o + 7]));
        }
    }
    static void DownmixF32_6CHToStereoS16(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i * 6;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = OrbisFloatToS16(DOWNMIX_FRONT * s[o + FL] + center + lfe +
                                           DOWNMIX_SURROUND * s[o + 4]);
            d[i * 2 + 1] = OrbisFloatToS16(DOWNMIX_FRONT * s[o + FR] + center + lfe +
                                           DOWNMIX_SURROUND * s[o + 5]);
        }
    }

    void FoldLfeIntoFronts() {
        constexpr float LFE_GAIN = 0.7071f;
        if (use_native_float) {
            float* d = al_buffer_float.data();
            for (u32 i = 0; i < buffer_frames; i++) {
                float* f = d + static_cast<size_t>(i) * num_channels;
                const float lfe = f[LF] * LFE_GAIN;
                f[FL] += lfe;
                f[FR] += lfe;
                f[LF] = 0.0f;
            }
        } else {
            s16* d = al_buffer_s16.data();
            for (u32 i = 0; i < buffer_frames; i++) {
                s16* f = d + static_cast<size_t>(i) * num_channels;
                const float lfe = static_cast<float>(f[LF]) * LFE_GAIN;
                f[FL] = ClampSampleToS16(static_cast<float>(f[FL]) + lfe);
                f[FR] = ClampSampleToS16(static_cast<float>(f[FR]) + lfe);
                f[LF] = 0;
            }
        }
    }

    static void DownmixS16_6CHToStereoF32(const void* src, void* dst, u32 frames, const float*) {
        constexpr float INV = 1.0f / 32768.0f;
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i * 6;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] =
                (DOWNMIX_FRONT * s[o + FL] + center + lfe + DOWNMIX_SURROUND * s[o + 4]) * INV;
            d[i * 2 + 1] =
                (DOWNMIX_FRONT * s[o + FR] + center + lfe + DOWNMIX_SURROUND * s[o + 5]) * INV;
        }
    }
    static void DownmixS16_8CHToStereoF32(const void* src, void* dst, u32 frames, const float*) {
        constexpr float INV = 1.0f / 32768.0f;
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i << 3;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = (DOWNMIX_FRONT * s[o + FL] + center + lfe +
                            DOWNMIX_SURROUND * (s[o + 4] + s[o + 6])) *
                           INV;
            d[i * 2 + 1] = (DOWNMIX_FRONT * s[o + FR] + center + lfe +
                            DOWNMIX_SURROUND * (s[o + 5] + s[o + 7])) *
                           INV;
        }
    }
    static void DownmixF32_6CHToStereoF32(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i * 6;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = DOWNMIX_FRONT * s[o + FL] + center + lfe + DOWNMIX_SURROUND * s[o + 4];
            d[i * 2 + 1] = DOWNMIX_FRONT * s[o + FR] + center + lfe + DOWNMIX_SURROUND * s[o + 5];
        }
    }
    static void DownmixF32_8CHToStereoF32(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i << 3;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] =
                DOWNMIX_FRONT * s[o + FL] + center + lfe + DOWNMIX_SURROUND * (s[o + 4] + s[o + 6]);
            d[i * 2 + 1] =
                DOWNMIX_FRONT * s[o + FR] + center + lfe + DOWNMIX_SURROUND * (s[o + 5] + s[o + 7]);
        }
    }

    static void DownmixF32_8CHToStereoS16(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);
        for (u32 i = 0; i < frames; i++) {
            const u32 o = i << 3;
            const float center = DOWNMIX_CENTER * s[o + FC];
            const float lfe = DOWNMIX_LFE * s[o + LF];
            d[i * 2 + 0] = OrbisFloatToS16(DOWNMIX_FRONT * s[o + FL] + center + lfe +
                                           DOWNMIX_SURROUND * (s[o + 4] + s[o + 6]));
            d[i * 2 + 1] = OrbisFloatToS16(DOWNMIX_FRONT * s[o + FR] + center + lfe +
                                           DOWNMIX_SURROUND * (s[o + 5] + s[o + 7]));
        }
    }

    // Float passthrough converters (for AL_EXT_FLOAT32)
    static void ConvertF32Mono(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);
        std::memcpy(d, s, frames * sizeof(float));
    }

    static void ConvertF32Stereo(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);
        std::memcpy(d, s, frames * 2 * sizeof(float));
    }

    static void ConvertF32_8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);
        std::memcpy(d, s, frames * 8 * sizeof(float));
    }

    static void ConvertF32Std8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);

        for (u32 i = 0; i < frames; i++) {
            const u32 offset = i << 3;
            d[offset + FL] = s[offset + FL];
            d[offset + FR] = s[offset + FR];
            d[offset + FC] = s[offset + FC];
            d[offset + LF] = s[offset + LF];
            d[offset + SL] = s[offset + STD_SL];
            d[offset + SR] = s[offset + STD_SR];
            d[offset + BL] = s[offset + STD_BL];
            d[offset + BR] = s[offset + STD_BR];
        }
    }

    // Float to S16 converters for OpenAL
    static void ConvertF32ToS16Mono(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        for (u32 i = 0; i < frames; i++)
            d[i] = OrbisFloatToS16(s[i]);
    }
#ifdef HAS_SSE2
    static void ConvertF32ToS16StereoSIMD(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        const __m128 scale = _mm_set1_ps(32768.0f);
        const __m128 min_val = _mm_set1_ps(-32768.0f);
        const __m128 max_val = _mm_set1_ps(32767.0f);

        const u32 num_samples = frames << 1;
        u32 i = 0;

        // Process 8 samples at a time
        for (; i + 8 <= num_samples; i += 8) {
            // Load 8 floats
            __m128 f1 = _mm_loadu_ps(&s[i]);
            __m128 f2 = _mm_loadu_ps(&s[i + 4]);

            // Scale and clamp
            f1 = _mm_mul_ps(f1, scale);
            f2 = _mm_mul_ps(f2, scale);
            f1 = _mm_max_ps(f1, min_val);
            f2 = _mm_max_ps(f2, min_val);
            f1 = _mm_min_ps(f1, max_val);
            f2 = _mm_min_ps(f2, max_val);

            // Convert to int32
            __m128i i1 = _mm_cvtps_epi32(f1);
            __m128i i2 = _mm_cvtps_epi32(f2);

            // Pack to int16
            __m128i packed = _mm_packs_epi32(i1, i2);

            // Store
            _mm_storeu_si128(reinterpret_cast<__m128i*>(&d[i]), packed);
        }

        // Handle remaining samples
        for (; i < num_samples; i++) {
            d[i] = OrbisFloatToS16(s[i]);
        }
    }
#else
    static void ConvertF32ToS16Stereo(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        const u32 num_samples = frames << 1;
        for (u32 i = 0; i < num_samples; i++)
            d[i] = OrbisFloatToS16(s[i]);
    }
#endif

#ifdef HAS_SSE2
    static void ConvertF32ToS16_8CH_SIMD(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        const __m128 scale = _mm_set1_ps(32768.0f);
        const __m128 min_val = _mm_set1_ps(-32768.0f);
        const __m128 max_val = _mm_set1_ps(32767.0f);

        const u32 num_samples = frames << 3;
        u32 i = 0;

        // Process 8 samples at a time (1 frame of 8CH audio)
        for (; i + 8 <= num_samples; i += 8) {
            __m128 f1 = _mm_loadu_ps(&s[i]);
            __m128 f2 = _mm_loadu_ps(&s[i + 4]);

            f1 = _mm_mul_ps(f1, scale);
            f2 = _mm_mul_ps(f2, scale);
            f1 = _mm_max_ps(_mm_min_ps(f1, max_val), min_val);
            f2 = _mm_max_ps(_mm_min_ps(f2, max_val), min_val);

            __m128i i1 = _mm_cvtps_epi32(f1);
            __m128i i2 = _mm_cvtps_epi32(f2);
            __m128i packed = _mm_packs_epi32(i1, i2);

            _mm_storeu_si128(reinterpret_cast<__m128i*>(&d[i]), packed);
        }

        for (; i < num_samples; i++) {
            d[i] = OrbisFloatToS16(s[i]);
        }
    }
#else
    static void ConvertF32ToS16_8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        const u32 num_samples = frames << 3;
        for (u32 i = 0; i < num_samples; i++)
            d[i] = OrbisFloatToS16(s[i]);
    }
#endif
    static void ConvertF32ToS16Std8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        s16* d = static_cast<s16*>(dst);

        for (u32 i = 0; i < frames; i++) {
            const u32 offset = i << 3;

            d[offset + FL] = OrbisFloatToS16(s[offset + FL]);
            d[offset + FR] = OrbisFloatToS16(s[offset + FR]);
            d[offset + FC] = OrbisFloatToS16(s[offset + FC]);
            d[offset + LF] = OrbisFloatToS16(s[offset + LF]);
            d[offset + SL] = OrbisFloatToS16(s[offset + STD_SL]);
            d[offset + SR] = OrbisFloatToS16(s[offset + STD_SR]);
            d[offset + BL] = OrbisFloatToS16(s[offset + STD_BL]);
            d[offset + BR] = OrbisFloatToS16(s[offset + STD_BR]);
        }
    }

    // Audio format parameters
    const u32 frame_size;
    const u32 guest_buffer_size;
    const u32 buffer_frames;
    const u32 sample_rate;
    const u32 num_channels;
    const bool is_float;
    const bool is_std;
    const std::array<int, 8> channel_layout;

    alignas(64) u64 period_us{0};
    bool submission_failed{false}; // report the first failure in a run
    bool pace_by_period{false}; // output thread only
    // audio_lead_ms: the buffers kept queued (0: the old timing) and the pool's size.
    u32 lead_buffers{0};
    u32 pool_buffers{NUM_BUFFERS};
    alignas(64) std::atomic<u64> last_output_time{0};
    u64 next_output_time{0};
    u64 last_volume_check_time{0};
    u32 output_count{0};
    u32 dropped_full{0}, ran_dry{0}; // since glitch_log_us
    u64 glitch_log_us{0};

    // OpenAL objects
    OpenALDevice* device_context{nullptr};
    u32 seen_reopen{0};
    ALuint source{0};
    std::vector<ALuint> buffers;
    std::vector<ALuint> available_buffers;
    ALenum format{AL_FORMAT_STEREO16};

    // Buffer management
    u32 buffer_size_bytes{0};
    std::vector<s16> al_buffer_s16;     // For S16 formats
    std::vector<float> al_buffer_float; // For float formats

    // Extension support
    bool has_float_ext{false};
    bool use_native_float{false};
    bool downmix_to_stereo{false};
    bool fold_lfe{false};
    // Left-handed play: the session's mirror, fixed when the port opens (VR::MirrorWorld).
    const bool mirror_sides{VR::MirrorWorld()};
    bool hrtf_active{false};

    // Converter function pointer
    ConverterFunc convert{nullptr};

    // Volume management
    alignas(64) std::atomic<float> current_gain{1.0f};
    std::atomic<float> game_gain{1.0f};

    std::string device_name;
    bool device_registered;
};

OpenALAudioOut::OpenALAudioOut(bool binaural) {
    OpenALDevice::force_binaural.store(binaural, std::memory_order_relaxed);
}

std::unique_ptr<PortBackend> OpenALAudioOut::Open(PortOut& port) {
    return std::make_unique<OpenALPortBackend>(port);
}

} // namespace Libraries::AudioOut