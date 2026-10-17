// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <libatrac9.h>

#include "core/vr/fsb_sample.h"

namespace VR::Fsb {
namespace {

template <class T>
bool ReadAt(std::span<const std::uint8_t> b, std::size_t at, T& out) {
    if (at > b.size() || b.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, b.data() + at, sizeof(T));
    return true;
}

struct Sample {
    std::uint32_t channels{1};
    std::uint64_t offset{0};
    std::uint32_t samples{0};
    std::array<std::uint8_t, 4> config{};
    bool have_config{false};
};

} // namespace

std::vector<char> DecodeSample(std::span<const std::uint8_t> bank, std::string_view name) {
    std::uint32_t version = 0, count = 0, headers = 0, names = 0, data_size = 0;
    if (bank.size() < 64 || std::memcmp(bank.data(), "FSB5", 4) != 0 ||
        !ReadAt(bank, 4, version) || !ReadAt(bank, 8, count) || !ReadAt(bank, 12, headers) ||
        !ReadAt(bank, 16, names) || !ReadAt(bank, 20, data_size) || count == 0 || count > 65536) {
        return {};
    }
    const std::size_t header = version == 1 ? 60 : 64;
    std::vector<Sample> samples(count);
    std::size_t p = header;
    for (Sample& s : samples) {
        std::uint64_t mode = 0;
        if (!ReadAt(bank, p, mode)) {
            return {};
        }
        p += 8;
        static constexpr std::array<std::uint32_t, 4> Channels{1, 2, 6, 8};
        s.channels = Channels[(mode >> 5) & 3];
        s.offset = ((mode >> 7) & 0x7FFFFFF) << 5;
        s.samples = static_cast<std::uint32_t>(mode >> 34);
        bool more = (mode & 1) != 0;
        while (more) {
            std::uint32_t chunk = 0;
            if (!ReadAt(bank, p, chunk)) {
                return {};
            }
            p += 4;
            more = (chunk & 1) != 0;
            const std::uint32_t size = (chunk >> 1) & 0xFFFFFF;
            const std::uint32_t type = (chunk >> 25) & 0x7F;
            if (type == 9 && size >= 8 && p + 8 <= bank.size()) {
                std::memcpy(s.config.data(), bank.data() + p + 4, 4);
                s.have_config = true;
            }
            p += size;
        }
    }
    const std::size_t name_table = header + headers;
    const std::size_t data = name_table + names;
    std::size_t index = count;
    for (std::uint32_t i = 0; i < count && index == count; ++i) {
        std::uint32_t off = 0;
        if (!ReadAt(bank, name_table + 4 * i, off) || name_table + off >= bank.size()) {
            return {};
        }
        const char* s = reinterpret_cast<const char*>(bank.data() + name_table + off);
        const std::size_t room = bank.size() - (name_table + off);
        if (std::string_view{s, strnlen(s, room)} == name) {
            index = i;
        }
    }
    if (index == count || !samples[index].have_config) {
        return {};
    }
    const Sample& s = samples[index];
    std::uint64_t end = data_size;
    for (const Sample& o : samples) {
        if (o.offset > s.offset && o.offset < end) {
            end = o.offset;
        }
    }
    if (data + end > bank.size() || s.offset >= end) {
        return {};
    }
    void* handle = Atrac9GetHandle();
    if (handle == nullptr) {
        return {};
    }
    std::array<std::uint8_t, 4> config = s.config;
    Atrac9CodecInfo info{};
    if (Atrac9InitDecoder(handle, config.data()) != 0 || Atrac9GetCodecInfo(handle, &info) != 0 ||
        info.channels < 1 || info.channels > 8 || info.frameSamples <= 0 ||
        info.frameSamples > 4096 || info.samplingRate <= 0) {
        Atrac9ReleaseHandle(handle);
        return {};
    }
    // Superframe by superframe (fixed size), each frame in it in turn; mono (the channels averaged).
    std::vector<float> mono;
    mono.reserve(s.samples);
    std::vector<short> frame(static_cast<std::size_t>(info.frameSamples * info.channels));
    const std::size_t super = static_cast<std::size_t>(std::max(info.superframeSize, 1));
    const int frames = std::max(info.framesInSuperframe, 1);
    const std::uint8_t* base = bank.data() + data + s.offset;
    const std::size_t total = static_cast<std::size_t>(end - s.offset);
    bool bad = false;
    for (std::size_t at = 0; at + super <= total && !bad && mono.size() < s.samples; at += super) {
        std::size_t in = 0;
        for (int f = 0; f < frames && mono.size() < s.samples; ++f) {
            int used = 0;
            if (Atrac9Decode(handle, base + at + in, static_cast<int>(super - in), frame.data(),
                             &used, 0) != 0 ||
                used <= 0 || in + static_cast<std::size_t>(used) > super) {
                bad = true;
                break;
            }
            in += static_cast<std::size_t>(used);
            for (int i = 0; i < info.frameSamples && mono.size() < s.samples; ++i) {
                float v = 0.0f;
                for (int c = 0; c < info.channels; ++c) {
                    v += frame[static_cast<std::size_t>(i * info.channels + c)];
                }
                mono.push_back(v / static_cast<float>(info.channels));
            }
        }
    }
    Atrac9ReleaseHandle(handle);
    if (mono.size() < 2) {
        return {};
    }
    // To 24 kHz (the mixers' rate): a 48 kHz bank's pairs averaged, else linear.
    const double step = static_cast<double>(info.samplingRate) / 24000.0;
    const std::size_t n = static_cast<std::size_t>(static_cast<double>(mono.size()) / step);
    std::vector<char> pcm(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        float v;
        if (info.samplingRate == 48000 && 2 * i + 1 < mono.size()) {
            v = 0.5f * (mono[2 * i] + mono[2 * i + 1]);
        } else {
            const double x = static_cast<double>(i) * step;
            const std::size_t j = std::min(static_cast<std::size_t>(x), mono.size() - 2);
            const float t = static_cast<float>(x - static_cast<double>(j));
            v = mono[j] + (mono[j + 1] - mono[j]) * t;
        }
        const auto q = static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
        pcm[2 * i] = static_cast<char>(q & 0xFF);
        pcm[2 * i + 1] = static_cast<char>((q >> 8) & 0xFF);
    }
    return pcm;
}

} // namespace VR::Fsb
