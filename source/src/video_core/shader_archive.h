// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The game's own shader programs, read from the player's dump (dvdroot_ps4/shader/
// *.shaderbnd.dcx: DCX-wrapped zlib, a BND4 of PSSL binaries; and the executable, eboot.bin,
// which holds the engine's own shaders laid out the same way) for the startup shader build
// (vk_shader_prebuild.cpp). Each binary ends its GCN code with the OrbShdr footer the emulator
// reads at draw time (AmdGpu::BinaryInfo): its `length` bytes of code sit right before it and its
// `shader_hash` is the program hash the pipeline cache names the program by. Nothing from the
// game is stored: only the programs asked for are kept, in memory, while the build runs.
//
// The parsing is pure (no files, no emulator state) so the unit tests (tests/
// test_shader_archive.cpp) run it on synthetic archives. Every read is bounds-checked: a damaged
// or unexpected file yields nothing, never an out-of-range read.

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"

namespace VideoCore::ShaderArchive {

// Largest decompressed archive (or executable) accepted (the biggest archive, gxflvershader, is
// 71 MiB; the executable 90 MiB).
inline constexpr u64 MaxArchiveBytes = 512ull << 20;

// Most bytes accepted between a program's code and its footer (the archives' largest is 152).
inline constexpr size_t MaxPadding = 4096;

// A DCX (DFLT) file's payload, inflated; nullopt for anything else or a damaged stream.
std::optional<std::vector<u8>> Inflate(std::span<const u8> dcx);

// Each entry of a BND4 container, in order (false: not a BND4 or out of bounds; the entries
// reported before the damage stand).
bool ForEachEntry(std::span<const u8> bnd4, const std::function<void(std::span<const u8>)>& entry);

// BinaryInfo::type of the footer. A geometry entry (.gpo) holds two programs under one hash: the
// geometry shader, then its copy shader (GsRuntimeInfo::vs_copy, AmdGpu GetParams of the VS copy
// stage).
inline constexpr u32 CopyShaderType = 6;

struct Program {
    u64 hash{};
    u32 type{};           // BinaryInfo::type (0 pixel, 1-3 vertex, 4 compute, 5 geometry, 6 copy)
    size_t code_offset{}; // in the entry
    size_t code_size{};   // bytes, a whole number of dwords
};
// The program whose footer is the first at or after `from` in one entry: its footer's hash and
// type and where its code is (nullopt without a footer, or with a program that does not open
// with s_mov_b32 vcc_hi naming that footer or does not fit before it). `next` (optional) gets
// where to look for the entry's next program.
std::optional<Program> ProgramOf(std::span<const u8> entry, size_t from = 0,
                                 size_t* next = nullptr);

class Programs {
public:
    // Reads every *.shaderbnd.dcx in `dir` (not the debug archives) and keeps the code of the
    // programs in `wanted` (all of them when empty). The number of archives read.
    size_t Load(const std::filesystem::path& dir, const std::unordered_set<u64>& wanted);
    // Adds one archive's programs (Load's step; the tests call it with synthetic data).
    void Add(std::span<const u8> dcx, const std::unordered_set<u64>& wanted);
    // Reads the executable (uncompressed) and adds the programs in `wanted` it holds; false when
    // the file cannot be read. AddExecutable is its step.
    bool LoadExecutable(const std::filesystem::path& file, const std::unordered_set<u64>& wanted);
    void AddExecutable(std::span<const u8> image, const std::unordered_set<u64>& wanted);
    bool Has(u64 hash) const {
        return code.contains(hash) || copy_code.contains(hash);
    }

    // The program's code as dwords, or empty; a geometry shader's copy shader by its hash.
    std::span<const u32> Code(u64 hash) const;
    std::span<const u32> CopyCode(u64 hash) const;
    size_t Count() const {
        return code.size();
    }
    size_t Bytes() const {
        return bytes;
    }

private:
    void Take(std::span<const u8> data, const std::unordered_set<u64>& wanted);

    std::unordered_map<u64, std::vector<u32>> code;
    std::unordered_map<u64, std::vector<u32>> copy_code;
    size_t bytes{};
};

} // namespace VideoCore::ShaderArchive
