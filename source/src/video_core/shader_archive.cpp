// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <zlib.h>

#include "video_core/shader_archive.h"

namespace VideoCore::ShaderArchive {
namespace {

// Little- and big-endian reads that refuse to leave the span.
template <typename T>
bool ReadLe(std::span<const u8> data, size_t at, T& out) {
    if (at > data.size() || data.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data.data() + at, sizeof(T));
    return true;
}

bool ReadBe32(std::span<const u8> data, size_t at, u32& out) {
    if (at > data.size() || data.size() - at < 4) {
        return false;
    }
    const u8* p = data.data() + at;
    out = (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
    return true;
}

size_t Find(std::span<const u8> data, std::string_view tag, size_t from = 0) {
    if (data.size() < tag.size()) {
        return std::string_view::npos;
    }
    const auto* begin = data.data();
    // Boyer-Moore-Horspool: the executable is searched whole (Programs::AddExecutable).
    const auto* pattern = reinterpret_cast<const u8*>(tag.data());
    const auto it = std::search(begin + std::min(from, data.size()), begin + data.size(),
                                std::boyer_moore_horspool_searcher(pattern, pattern + tag.size()));
    return it == begin + data.size() ? std::string_view::npos : size_t(it - begin);
}

constexpr std::string_view Footer{"OrbShdr", 7};

} // namespace

std::optional<std::vector<u8>> Inflate(std::span<const u8> dcx) {
    // DCX: "DCX\0" ... "DCS\0" u32 BE uncompressed, u32 BE compressed ... "DCA\0" u32 BE 8, the
    // zlib stream.
    if (dcx.size() < 4 || std::memcmp(dcx.data(), "DCX\0", 4) != 0) {
        return std::nullopt;
    }
    const size_t dcs = Find(dcx.first(std::min<size_t>(dcx.size(), 256)), {"DCS\0", 4});
    const size_t dca = Find(dcx.first(std::min<size_t>(dcx.size(), 256)), {"DCA\0", 4});
    u32 unpacked = 0, header = 0;
    if (dcs == std::string_view::npos || dca == std::string_view::npos ||
        !ReadBe32(dcx, dcs + 4, unpacked) || !ReadBe32(dcx, dca + 4, header) || unpacked == 0 ||
        unpacked > MaxArchiveBytes || header < 8 || dca + header > dcx.size()) {
        return std::nullopt;
    }
    const auto stream = dcx.subspan(dca + header);
    std::vector<u8> out(unpacked);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) {
        return std::nullopt;
    }
    z.next_in = const_cast<Bytef*>(stream.data());
    z.avail_in = static_cast<uInt>(std::min<size_t>(stream.size(), UINT32_MAX));
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&z, Z_FINISH);
    const size_t produced = out.size() - z.avail_out;
    inflateEnd(&z);
    if (result != Z_STREAM_END || produced != out.size()) {
        return std::nullopt;
    }
    return out;
}

bool ForEachEntry(std::span<const u8> bnd4, const std::function<void(std::span<const u8>)>& entry) {
    s32 count = 0;
    s64 header_size = 0;
    if (bnd4.size() < 64 || std::memcmp(bnd4.data(), "BND4", 4) != 0 || !ReadLe(bnd4, 12, count) ||
        !ReadLe(bnd4, 32, header_size) || count < 0 || header_size < 28 || header_size > 4096) {
        return false;
    }
    for (s32 i = 0; i < count; ++i) {
        const size_t at = 64 + size_t(i) * size_t(header_size);
        s64 size = 0;
        u32 offset = 0;
        if (!ReadLe(bnd4, at + 8, size) || !ReadLe(bnd4, at + 24, offset) || size < 0 ||
            offset > bnd4.size() || u64(size) > bnd4.size() - offset) {
            return false;
        }
        entry(bnd4.subspan(offset, size_t(size)));
    }
    return true;
}

std::optional<Program> ProgramOf(std::span<const u8> entry, size_t from, size_t* next) {
    const size_t footer = Find(entry, Footer, from);
    if (next) {
        *next = footer == std::string_view::npos ? entry.size() : footer + Footer.size();
    }
    u32 bits = 0;
    u64 hash = 0;
    if (footer == std::string_view::npos || !ReadLe(entry, footer + 8, bits) ||
        !ReadLe(entry, footer + 16, hash)) {
        return std::nullopt;
    }
    // BinaryInfo: pssl_or_cg:1 cached:1 type:4 source_type:2 length:24. The length is the
    // program's bytes from its first instruction. The program opens with s_mov_b32 vcc_hi, #imm
    // whose immediate places the footer (imm + 1) * 8 bytes after it, as the emulator finds it at
    // draw time (AmdGpu GetParams); padding (at most MaxPadding) may sit between the code's end
    // and the footer. A program without that opening is not taken.
    const u32 type = (bits >> 2) & 0xF;
    const size_t length = bits >> 8;
    if (length < 8 || length % 4 != 0 || length > footer) {
        return std::nullopt;
    }
    // The opening sits a whole number of 8-byte steps before the footer with the program fitting
    // between them: the nearest such opening.
    const size_t farthest = std::min(footer, length + MaxPadding);
    for (size_t step = (length + 7) / 8; step * 8 <= farthest; ++step) {
        const size_t at = footer - step * 8;
        u32 first = 0, imm = 0;
        if (ReadLe(entry, at, first) && ReadLe(entry, at + 4, imm) && first == 0xBEEB03FFu &&
            u64{imm} + 1 == step) {
            return Program{hash, type, at, length};
        }
    }
    return std::nullopt;
}

void Programs::Take(std::span<const u8> data, const std::unordered_set<u64>& wanted) {
    for (size_t from = 0; from < data.size();) {
        const auto program = ProgramOf(data, from, &from);
        if (!program || (!wanted.empty() && !wanted.contains(program->hash))) {
            continue;
        }
        auto& map = program->type == CopyShaderType ? copy_code : code;
        if (map.contains(program->hash)) {
            continue;
        }
        std::vector<u32> words(program->code_size / 4);
        std::memcpy(words.data(), data.data() + program->code_offset, program->code_size);
        bytes += program->code_size;
        map.emplace(program->hash, std::move(words));
    }
}

void Programs::Add(std::span<const u8> dcx, const std::unordered_set<u64>& wanted) {
    const auto bnd4 = Inflate(dcx);
    if (!bnd4) {
        return;
    }
    // Every program of each entry (a geometry entry holds two).
    ForEachEntry(*bnd4, [&](std::span<const u8> entry) { Take(entry, wanted); });
}

void Programs::AddExecutable(std::span<const u8> image, const std::unordered_set<u64>& wanted) {
    Take(image, wanted);
}

bool Programs::LoadExecutable(const std::filesystem::path& file,
                              const std::unordered_set<u64>& wanted) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > MaxArchiveBytes) {
        return false;
    }
    std::vector<u8> data(size);
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size))) {
        return false;
    }
    AddExecutable(data, wanted);
    return true;
}

size_t Programs::Load(const std::filesystem::path& dir, const std::unordered_set<u64>& wanted) {
    std::error_code error;
    size_t archives = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir, error)) {
        const auto name = file.path().filename().string();
        if (!file.is_regular_file(error) || !name.ends_with(".shaderbnd.dcx")) {
            continue; // the *.shaderbnddebug.dcx archives hold development variants only
        }
        const auto size = file.file_size(error);
        if (error || size == 0 || size > MaxArchiveBytes) {
            continue;
        }
        std::vector<u8> data(size);
        std::ifstream in(file.path(), std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size))) {
            continue;
        }
        Add(data, wanted);
        ++archives;
    }
    return archives;
}

std::span<const u32> Programs::Code(u64 hash) const {
    const auto it = code.find(hash);
    return it == code.end() ? std::span<const u32>{} : std::span<const u32>{it->second};
}

std::span<const u32> Programs::CopyCode(u64 hash) const {
    const auto it = copy_code.find(hash);
    return it == copy_code.end() ? std::span<const u32>{} : std::span<const u32>{it->second};
}

} // namespace VideoCore::ShaderArchive
