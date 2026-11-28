// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Weapon voice lines (2026-11-23: "a voice over that says how to use the weapon - make
// it lore friendly ... First time equipping the weapon should play that line", "If an audio track
// is playing and we switch weapons it should stop", "It should be extremely performant ... on
// subsequent weapon equips it should add no latency or extra operations").
//
// One recorded line per right-hand weapon with a non-standard interaction (Lines; the tester: "Left
// hand weapons should have no lines"; a left-hand weapon's change never stops one either):
// voice/<key>.mp3 or .wav next to the emulator
// (VR_VOICE_DIR for tests); the words and who says them are assets/voice/lines.md. Three threads,
// sharing only two published values:
//  - the game thread (bloodborne_body.cpp PublishStatus, every game frame): the Director. Its idle
//    path is one compare (the right hand's weapon family unchanged). A change looks the family up
//    (an array index) and tests one bit; only a weapon whose line was never heard does more.
//  - a worker, started by the first such weapon: reads and decodes that one file, once a session.
//  - the audio thread (audioout.cpp): the Mixer, told the Director's latest Frame (a seqlock), and
//    reporting which line reached three quarters and which command ended.
// A line counts as heard when it plays to its end, or when you switch weapons after 3/4 of it (cut
// sooner, it plays again the next time that weapon comes to hand). vr.ini: weapon_voice (1),
// weapon_voice_volume (%), weapon_voice_heard (the heard lines' keys).
//
// Pure (no emulator dependencies; tests/test_weapon_voice.cpp).
namespace VR::WeaponVoice {

struct Line {
    std::string_view key; // its file: voice/<key>.mp3 or voice/<key>.wav (bb_weapons.h's key)
    std::int16_t family;  // EquipParamWeapon id / 100000 (bb_weapons.h)
};

// The right-hand weapons with a non-standard interaction (launcher/controls_guide.cpp's words; the
// user, 2026-11-23: "weapons that are simple, like the crane whip, beast saw, etc do not need a
// voice over. Only complicated ones do"): the other hand on the handle to transform, the blades
// pulled apart, the stab into the chest, the raised mace, the engine, the bow, the spin buffs, the
// tentacles, the moonlight's sweep, the scabbard. A squeeze or a held grip to transform (Saw
// Cleaver, Threaded Cane, Beast Claw...), a trigger's shot or charge (Reiterpallasch, Stake Driver)
// have none; the left hand's (guns, torches, shields) and the fists have none; nor has the Hunter
// Axe (the tester: "I want no voice for the hunter axe").
// (vr.ini keeps heard lines by key, so the order may change.)
inline constexpr std::array<Line, 16> Lines{{
    {"burial_blade", 51},
    {"blade_of_mercy", 40},
    {"rakuyo", 270},
    {"simons_bowblade", 320},
    {"kirkhammer", 80},
    {"ludwigs_holy_blade", 81},
    {"rifle_spear", 100},
    {"holy_moonlight_sword", 260},
    {"logarius_wheel", 120},
    {"bloodletter", 290},
    {"tonitrus", 130},
    {"chikage", 20},
    {"amygdalan_arm", 250},
    {"kos_parasite", 380},
    {"boom_hammer", 280},
    {"whirligig_saw", 310},
}};
inline constexpr int LineCount = static_cast<int>(Lines.size());
static_assert(LineCount <= 64, "a heard line is one bit of a 64-bit mask");

// The line of each family below MaxFamily (an array index, no search).
inline constexpr int MaxFamily = 400;
inline constexpr std::array<std::int8_t, MaxFamily> FamilyLines = [] {
    std::array<std::int8_t, MaxFamily> out{};
    out.fill(-1);
    for (int i = 0; i < LineCount; ++i) {
        const std::int16_t f = Lines[static_cast<std::size_t>(i)].family;
        if (f >= 0 && f < MaxFamily) {
            out[static_cast<std::size_t>(f)] = static_cast<std::int8_t>(i);
        }
    }
    return out;
}();

// A weapon family's line, or -1 (no line, or not a weapon: -1 while the game loads).
constexpr int LineOf(std::int32_t family) {
    return family >= 0 && family < MaxFamily ? FamilyLines[static_cast<std::size_t>(family)] : -1;
}

// vr.ini weapon_voice_heard: the heard lines' keys, comma separated (an unknown key is dropped).
inline std::uint64_t ParseHeard(std::string_view text) {
    std::uint64_t mask = 0;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view key = text.substr(0, comma);
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
            key.remove_prefix(1);
        }
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r')) {
            key.remove_suffix(1);
        }
        for (int i = 0; i < LineCount; ++i) {
            if (Lines[static_cast<std::size_t>(i)].key == key) {
                mask |= std::uint64_t{1} << i;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return mask;
}
inline std::string FormatHeard(std::uint64_t mask) {
    std::string out;
    for (int i = 0; i < LineCount; ++i) {
        if ((mask >> i) & 1) {
            if (!out.empty()) {
                out += ',';
            }
            out += Lines[static_cast<std::size_t>(i)].key;
        }
    }
    return out;
}

// ---- A decoded line ------------------------------------------------------------------------------

// Mono 16-bit PCM at the file's own rate (resampled as it plays).
inline constexpr std::size_t MaxClipSamples = 4 * 1024 * 1024; // 8 MiB of mono PCM
struct Clip {
    std::vector<std::int16_t> pcm;
    std::uint32_t rate{0};
    [[nodiscard]] std::int64_t LengthNs() const {
        return rate ? static_cast<std::int64_t>(pcm.size()) * 1'000'000'000 / rate : 0;
    }
};

// The bytes of an ID3v2 tag ahead of an MP3's frames (ElevenLabs writes one), or 0.
inline std::size_t Id3Size(std::span<const std::uint8_t> file) {
    if (file.size() < 10 || file[0] != 'I' || file[1] != 'D' || file[2] != '3') {
        return 0;
    }
    const std::size_t body = (std::size_t{file[6] & 0x7fu} << 21) |
                             (std::size_t{file[7] & 0x7fu} << 14) |
                             (std::size_t{file[8] & 0x7fu} << 7) | std::size_t{file[9] & 0x7fu};
    const std::size_t total = 10 + body + ((file[5] & 0x10) ? 10 : 0); // (+ a v2.4 footer)
    return total <= file.size() ? total : 0;
}

// Interleaved 16-bit frames of `channels` appended to `mono` as their average.
inline void AppendMono(const std::int16_t* frames, std::size_t count, int channels,
                       std::vector<std::int16_t>& mono) {
    if (count == 0 || channels <= 0) {
        return;
    }
    if (channels == 1) {
        mono.insert(mono.end(), frames, frames + count);
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += frames[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
        }
        mono.push_back(static_cast<std::int16_t>(sum / channels));
    }
}

// A RIFF WAVE file of 16-bit PCM (1..8 channels, mixed down to mono), or nothing.
inline std::optional<Clip> DecodeWav(std::span<const std::uint8_t> file) {
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(file[at] | (file[at + 1] << 8));
    };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    const auto tag = [&](std::size_t at, const char* t) {
        return file[at] == static_cast<std::uint8_t>(t[0]) &&
               file[at + 1] == static_cast<std::uint8_t>(t[1]) &&
               file[at + 2] == static_cast<std::uint8_t>(t[2]) &&
               file[at + 3] == static_cast<std::uint8_t>(t[3]);
    };
    if (file.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE")) {
        return std::nullopt;
    }
    int channels = 0;
    std::uint32_t rate = 0;
    std::size_t at = 12;
    while (at + 8 <= file.size()) {
        const std::uint32_t size = u32(at + 4);
        const std::size_t body = at + 8;
        if (size > file.size() - body) {
            return std::nullopt; // cut short
        }
        if (tag(at, "fmt ") && size >= 16) {
            const std::uint32_t format = u16(body);
            if (format == 0xFFFE) { // WAVEFORMATEXTENSIBLE must specify the PCM subtype.
                constexpr std::array<std::uint8_t, 16> pcm_guid{
                    1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
                if (size < 40 || u16(body + 16) < 22 || u16(body + 16) > size - 18 ||
                    !std::equal(pcm_guid.begin(), pcm_guid.end(), file.data() + body + 24)) {
                    return std::nullopt;
                }
            }
            channels = static_cast<int>(u16(body + 2));
            rate = u32(body + 4);
            const std::uint32_t bits = u16(body + 14);
            if ((format != 1 && format != 0xFFFE) || bits != 16 || channels < 1 || channels > 8 ||
                rate < 4000 || rate > 192000) {
                return std::nullopt;
            }
        } else if (tag(at, "data")) {
            if (!channels) {
                return std::nullopt; // data before its format
            }
            const std::size_t frames = size / (2 * static_cast<std::size_t>(channels));
            if (frames == 0 || frames > MaxClipSamples) {
                return std::nullopt;
            }
            Clip clip;
            clip.rate = rate;
            clip.pcm.reserve(frames);
            std::size_t sample = body;
            for (std::size_t i = 0; i < frames; ++i) {
                int sum = 0;
                for (int c = 0; c < channels; ++c, sample += 2) {
                    sum += static_cast<std::int16_t>(u16(sample));
                }
                clip.pcm.push_back(static_cast<std::int16_t>(sum / channels));
            }
            return clip;
        }
        at = body + size + (size & 1); // chunks are word aligned
    }
    return std::nullopt;
}

// ---- The game thread and the audio thread --------------------------------------------------------

inline constexpr std::uint8_t Stop = 0;
inline constexpr std::uint8_t Play = 1;

// The Director's latest command (one writer, the game thread).
struct Frame {
    std::uint64_t id{0}; // + 1 per command
    std::int64_t ns{0};  // when it was given (steady clock)
    float volume{1.0f};  // x the base gain (0..2)
    std::int8_t line{-1};
    std::uint8_t state{Stop};
};

// What the audio thread did with the commands (its own writes; the Director reads them).
struct Report {
    std::atomic<std::uint64_t> heard_id{0};   // three quarters actually mixed
    std::atomic<std::uint64_t> ended_id{0};   // played to its end, faded out or given up
    void Heard(std::uint64_t id) {
        heard_id.store(id, std::memory_order_release);
    }
    void Ended(std::uint64_t id) {
        ended_id.store(id, std::memory_order_release);
    }
};

// The game thread: which line to play, when to stop it, what has been heard. `Env` (duck typed,
// inlined) gives the rest:
//   std::uint64_t LoadHeard();      vr.ini's heard lines (asked once, at the first change)
//   bool Allowed();                 the controls the lines describe are in use (asked only for an
//                                   unheard weapon's line)
//   float Volume();
//   bool Missing(int line);         its file could not be read or decoded this session
//   std::uint64_t HeardId(); std::uint64_t EndedId(); (the Report)
//   void Publish(const Frame&);     to the audio thread
//   void Request(int line);         decode it (the env asks the worker once per line)
//   void SaveHeard(std::uint64_t);  vr.ini weapon_voice_heard
class Director {
public:
    explicit Director(std::uint64_t heard = 0) : heard_(heard) {}

    // Every game frame: the right hand's weapon family (-1: not known, as while the game loads:
    // never a change).
    template <class Env>
    void Tick(std::int32_t right, std::int64_t now, Env& env) {
        if (right == right_ || right < 0) {
            if (line_ >= 0) {
                Follow(env);
            }
            return;
        }
        right_ = right;
        if (!loaded_) {
            heard_ |= env.LoadHeard();
            loaded_ = true;
        }
        if (line_ >= 0) {
            Cut(now, env); // switching weapons stops it
        }
        if (const int line = Wanted(LineOf(right), env); line >= 0) {
            Start(line, now, env);
        }
    }

    [[nodiscard]] std::uint64_t Heard() const {
        return heard_;
    }
    [[nodiscard]] int Playing() const {
        return line_;
    }

private:
    template <class Env>
    int Wanted(int line, Env& env) const {
        return line >= 0 && !((heard_ >> line) & 1) && !env.Missing(line) && env.Allowed() ? line
                                                                                            : -1;
    }
    template <class Env>
    void Start(int line, std::int64_t now, Env& env) {
        const float volume = env.Volume();
        if (!(volume > 0.0f)) {
            return;
        }
        env.Request(line);
        line_ = line;
        env.Publish(Frame{++id_, now, volume, static_cast<std::int8_t>(line), Play});
    }
    template <class Env>
    void Mark(int line, Env& env) {
        heard_ |= std::uint64_t{1} << line;
        env.SaveHeard(heard_);
    }
    // Even a closed port or expired request can end a command. Credit only mixed audio.
    template <class Env>
    void Follow(Env& env) {
        if (env.EndedId() != id_) {
            return;
        }
        const int line = line_;
        line_ = -1;
        if (env.HeardId() == id_) {
            Mark(line, env);
        }
    }
    template <class Env>
    void Cut(std::int64_t now, Env& env) {
        const bool ended = env.EndedId() == id_;
        if (env.HeardId() == id_) {
            Mark(line_, env);
        }
        if (!ended) {
            env.Publish(Frame{++id_, now, 1.0f, -1, Stop});
        }
        line_ = -1;
    }

    std::int32_t right_{-2}; // the right hand's last known family (-2: none seen yet)
    std::uint64_t heard_{0};
    bool loaded_{false};
    std::uint64_t id_{0}; // the last command's
    int line_{-1};        // the line asked for (playing, or waiting for its file)
};

// The audio thread: plays the Director's line StartDelayNs (1 s) after its Play command, once its
// file is decoded, and fades it out (no click) when told to stop or when another replaces it.
// Allocation-free; with nothing to play a Mix is a few compares. `Clips` (duck typed): const
// Clip* Get(int line) (null until decoded) and bool Missing(int line).
class Mixer {
public:
    static constexpr float Gain = 0.9f;
    static constexpr float FadeS = 0.06f; // stopped or replaced: out over this
    static constexpr std::int64_t StartDelayNs = 1'000'000'000; // from first equip; decode meanwhile
    static constexpr std::int64_t WaitNs = 3'000'000'000; // a file not decoded by then: given up

    template <class Clips>
    void Mix(void* buffer, std::size_t frames, unsigned channels, unsigned rate, bool is_float,
             const Frame& frame, std::int64_t now, Clips& clips, Report& report) {
        if (frame.id != seen_) {
            seen_ = frame.id;
            // A reopened main port has a fresh mixer but shares the last command and report.
            // An ended (or abandoned) command stays retired even before the next game tick.
            want_ = frame.state == Play && report.ended_id.load(std::memory_order_acquire) != frame.id
                        ? frame.line : -1;
            want_id_ = frame.id;
            want_ns_ = frame.ns;
            want_volume_ = std::isfinite(frame.volume) ? std::clamp(frame.volume, 0.0f, 2.0f) : 1.0f;
            if (clip_ != nullptr) {
                fading_ = true; // a new line waits for this one's fade
            }
        }
        if (clip_ == nullptr && want_ >= 0) {
            const auto elapsed = now - want_ns_;
            if (elapsed < StartDelayNs) {
                return;
            }
            if (elapsed > WaitNs || clips.Missing(want_)) {
                report.Ended(want_id_);
                want_ = -1;
            } else if (const Clip* clip = clips.Get(want_); clip != nullptr && clip->rate && !clip->pcm.empty()) {
                clip_ = clip;
                id_ = want_id_;
                volume_ = want_volume_;
                pos_ = 0.0;
                gain_ = 1.0f;
                fading_ = false;
                want_ = -1;
                heard_at_ = static_cast<double>(clip->pcm.size()) * 0.75;
            }
        }
        if (clip_ == nullptr || buffer == nullptr || frames == 0 || rate == 0 ||
            (channels != 1 && channels != 2 && channels != 8)) {
            return;
        }
        const double step = static_cast<double>(clip_->rate) / rate;
        const float fade = 1.0f / (FadeS * static_cast<float>(rate));
        const bool done = is_float ? MixSamples(static_cast<float*>(buffer), frames, channels, step, fade)
                                   : MixSamples(static_cast<std::int16_t*>(buffer), frames, channels,
                                                step, fade);
        if (heard_at_ > 0.0 && pos_ >= heard_at_ && !fading_) {
            report.Heard(id_);
            heard_at_ = 0.0;
        }
        if (done) {
            clip_ = nullptr;
            report.Ended(id_);
        }
    }

    [[nodiscard]] bool Live() const {
        return clip_ != nullptr;
    }

    // The main port stops and joins before its replacement can open. Retire only the
    // line it actually started. A newer command can still play on the replacement port.
    void Close(Report& report) {
        if (clip_) {
            report.Ended(id_);
            clip_ = nullptr;
        }
    }

private:
    template <class T>
    static void Add(T& destination, float voice) {
        if constexpr (std::is_same_v<T, float>) {
            destination = std::clamp(destination + voice, -1.0f, 1.0f);
        } else {
            destination = static_cast<T>(
                std::clamp(std::lround(destination + voice * 32768.0f), -32768L, 32767L));
        }
    }
    // True when it ended (its last sample, or its fade).
    template <class T>
    bool MixSamples(T* output, std::size_t frames, unsigned channels, double step, float fade) {
        const std::size_t count = clip_->pcm.size();
        const float side = channels == 1 ? 1.0f : std::sqrt(0.5f); // centred, equal power
        const float level = Gain * volume_ * side / 32768.0f;
        for (std::size_t i = 0; i < frames; ++i) {
            const auto a = static_cast<std::size_t>(pos_);
            if (a >= count) {
                return true;
            }
            if (fading_) {
                gain_ -= fade;
                if (gain_ <= 0.0f) {
                    return true;
                }
            }
            const std::size_t b = a + 1 < count ? a + 1 : a;
            const float sa = clip_->pcm[a];
            const float s = sa + (static_cast<float>(clip_->pcm[b]) - sa) *
                                     static_cast<float>(pos_ - static_cast<double>(a));
            const float voice = s * level * gain_;
            Add(output[i * channels], voice);
            if (channels > 1) {
                Add(output[i * channels + 1], voice);
            }
            pos_ += step;
        }
        return static_cast<std::size_t>(pos_) >= count;
    }

    std::uint64_t seen_{0}; // the last command looked at
    int want_{-1};          // a line asked for, not started yet
    std::uint64_t want_id_{0};
    std::int64_t want_ns_{0};
    float want_volume_{1.0f};
    const Clip* clip_{nullptr}; // playing (never freed while the process runs)
    std::uint64_t id_{0};       // its command
    double pos_{0.0};
    double heard_at_{0.0};
    float volume_{1.0f};
    float gain_{1.0f};
    bool fading_{false};
};

// ---- Runtime (core/vr/weapon_voice.cpp) ----------------------------------------------------------

// The game thread, every game frame (bloodborne_body.cpp PublishStatus): the right hand's weapon
// family. One compare while it is unchanged.
void Tick(std::int32_t right, std::int64_t now);
// The audio thread, every buffer of the main port (audioout.cpp).
void Mix(Mixer& mixer, void* buffer, std::size_t frames, unsigned channels, unsigned rate,
         bool is_float, std::int64_t now);
void Close(Mixer& mixer);

} // namespace VR::WeaponVoice
