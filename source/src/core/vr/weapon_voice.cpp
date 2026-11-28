// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <minimp3.h> // (its code is built once, in core/libraries/np/trophy_ui.cpp)
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/vr/seqlock.h"
#include "core/vr/two_hand_settings.h"
#include "core/vr/vr_session.h"
#include "core/vr/weapon_voice.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::WeaponVoice {
namespace {

// The decoded lines: null until the worker has one (then never freed while the process runs: the
// audio thread may be playing it at exit; at most one per line a session).
std::array<std::atomic<const Clip*>, LineCount> g_clips{};
std::array<std::atomic<bool>, LineCount> g_missing{};
SeqLock<Frame> g_frame; // the Director's commands (the game thread writes)
Report g_report;        // the audio thread's

// voice/ next to the emulator (as dlss/), or VR_VOICE_DIR (tests).
std::filesystem::path VoiceDirectory() {
    if (const char* dir = std::getenv("VR_VOICE_DIR"); dir != nullptr && *dir != '\0') {
        return std::filesystem::path(dir);
    }
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) {
        return {};
    }
    path.resize(n);
    return std::filesystem::path(path).parent_path() / "voice";
#else
    return std::filesystem::current_path() / "voice";
#endif
}

std::optional<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > (std::uintmax_t{64} << 20)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<Clip> DecodeMp3(std::span<const std::uint8_t> file) {
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    std::vector<mp3d_sample_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    Clip clip;
    std::size_t at = Id3Size(file); // (ElevenLabs writes one: never read as frames)
    while (at < file.size()) {
        mp3dec_frame_info_t info{};
        const int left = static_cast<int>(std::min<std::size_t>(file.size() - at, 1u << 30));
        const int frames = mp3dec_decode_frame(&decoder, file.data() + at, left, pcm.data(), &info);
        if (info.frame_bytes <= 0) {
            break; // nothing more found
        }
        at += static_cast<std::size_t>(info.frame_bytes);
        if (frames <= 0 || info.channels <= 0) {
            continue; // a tag or junk skipped
        }
        if (clip.rate == 0) {
            clip.rate = static_cast<std::uint32_t>(info.hz);
        }
        if (static_cast<std::uint32_t>(info.hz) == clip.rate) {
            if (static_cast<std::size_t>(frames) > MaxClipSamples - clip.pcm.size()) {
                return std::nullopt;
            }
            AppendMono(pcm.data(), static_cast<std::size_t>(frames), info.channels, clip.pcm);
        }
    }
    if (clip.pcm.empty() || clip.rate == 0) {
        return std::nullopt;
    }
    return clip;
}

// The worker: one line's file read and decoded, then published to the audio thread.
void Decode(int line) {
    const std::string key{Lines[static_cast<std::size_t>(line)].key};
    const auto dir = VoiceDirectory();
    for (const char* extension : {".mp3", ".wav"}) {
        const auto path = dir / (key + extension);
        const auto bytes = ReadFile(path);
        if (!bytes) {
            continue;
        }
        auto clip = extension[1] == 'm' ? DecodeMp3(*bytes) : DecodeWav(*bytes);
        if (!clip) {
            LOG_WARNING(Core, "VR voice: {} could not be decoded (an MP3, or a 16-bit PCM WAV)",
                        Common::FS::PathToUTF8String(path));
            continue;
        }
        const Clip* kept = new Clip(std::move(*clip));
        g_clips[static_cast<std::size_t>(line)].store(kept, std::memory_order_release);
        LOG_INFO(Core, "VR voice: {} loaded ({:.1f} s, {} Hz)", Common::FS::PathToUTF8String(path),
                 static_cast<double>(kept->LengthNs()) * 1e-9, kept->rate);
        return;
    }
    g_missing[static_cast<std::size_t>(line)].store(true, std::memory_order_release);
    LOG_INFO(Core, "VR voice: no {}.mp3 or .wav in {}: that weapon stays silent",
             key, Common::FS::PathToUTF8String(dir));
}

// Started by the first line asked for; waits on its queue, so it costs nothing in between. Never
// destroyed (it may be decoding at exit).
struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<int> lines;
};
Worker& TheWorker() {
    static Worker* const worker = [] {
        auto owner = std::make_unique<Worker>();
        std::thread([w = owner.get()] {
            Common::SetCurrentThreadName("VR:Voice");
            for (;;) {
                int line = -1;
                {
                    std::unique_lock lock{w->mutex};
                    w->wake.wait(lock, [w] { return !w->lines.empty(); });
                    line = w->lines.front();
                    w->lines.pop_front();
                }
                try {
                    Decode(line);
                } catch (const std::bad_alloc&) {
                    g_missing[static_cast<std::size_t>(line)].store(true, std::memory_order_release);
                } catch (const std::filesystem::filesystem_error&) {
                    g_missing[static_cast<std::size_t>(line)].store(true, std::memory_order_release);
                }
            }
        }).detach();
        return owner.release();
    }();
    return *worker;
}

// The Director's world (the game thread only).
struct Env {
    std::array<bool, LineCount> requested{};

    std::uint64_t LoadHeard() const {
        return VR::GetSettings().weapon_voice_heard;
    }
    // The lines describe the default motion controls: the right grip transforms (not right_grip
    // draw), the Original two-handed controls, first person.
    bool Allowed() const {
        const auto& s = VR::GetSettings();
        return s.weapon_voice && !s.controls_classic && !s.right_grip_draw &&
               !TwoHand::GetTwoHandSettings().optimized_controls && VR::IsFirstPerson();
    }
    float Volume() const {
        return static_cast<float>(VR::GetSettings().weapon_voice_volume) * 0.01f;
    }
    bool Missing(int line) const {
        return g_missing[static_cast<std::size_t>(line)].load(std::memory_order_acquire);
    }
    std::uint64_t HeardId() const {
        return g_report.heard_id.load(std::memory_order_acquire);
    }
    std::uint64_t EndedId() const {
        return g_report.ended_id.load(std::memory_order_acquire);
    }
    void Publish(const Frame& frame) {
        g_frame.Store(frame);
        if (frame.state == Play) {
            LOG_INFO(Core, "VR voice: {} (the first time in hand)",
                     Lines[static_cast<std::size_t>(frame.line)].key);
        } else {
            LOG_INFO(Core, "VR voice: stopped (a weapon switched)");
        }
    }
    void Request(int line) {
        auto& asked = requested[static_cast<std::size_t>(line)];
        if (asked || g_clips[static_cast<std::size_t>(line)].load(std::memory_order_acquire)) {
            return;
        }
        asked = true;
        try {
            auto& worker = TheWorker();
            {
                std::lock_guard lock{worker.mutex};
                worker.lines.push_back(line);
            }
            worker.wake.notify_one();
        } catch (const std::bad_alloc&) {
            g_missing[static_cast<std::size_t>(line)].store(true, std::memory_order_release);
        } catch (const std::system_error&) {
            g_missing[static_cast<std::size_t>(line)].store(true, std::memory_order_release);
        }
    }
    void SaveHeard(std::uint64_t heard) {
        const std::string keys = FormatHeard(heard);
        LOG_INFO(Core, "VR voice: heard {}", keys);
        VR::SaveSetting("weapon_voice_heard", keys); // (the vr.ini writer's own thread)
    }
};

Director g_director;
Env g_env;

struct ClipStore {
    const Clip* Get(int line) const {
        return line >= 0 && line < LineCount
                   ? g_clips[static_cast<std::size_t>(line)].load(std::memory_order_acquire)
                   : nullptr;
    }
    bool Missing(int line) const {
        return line >= 0 && line < LineCount &&
               g_missing[static_cast<std::size_t>(line)].load(std::memory_order_acquire);
    }
};

} // Anonymous namespace

void Tick(std::int32_t right, std::int64_t now) {
    g_director.Tick(right, now, g_env);
}

void Mix(Mixer& mixer, void* buffer, std::size_t frames, unsigned channels, unsigned rate,
         bool is_float, std::int64_t now) {
    // A bounded seqlock read can overlap a command: keep this thread's last good frame.
    thread_local Frame frame{};
    g_frame.Load(frame, 4);
    ClipStore clips;
    mixer.Mix(buffer, frames, channels, rate, is_float, frame, now, clips, g_report);
}

void Close(Mixer& mixer) {
    mixer.Close(g_report);
}

} // namespace VR::WeaponVoice
