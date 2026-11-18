// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The shader cache's pack (2026-11-03): the cached shader blobs in one file, read at start-up
// through its index instead of one file each. On Windows the first open of each new file costs
// about 4 ms (the antivirus): a cache of 1,671 small files took 5.8 s to read the first time and
// 0.11 s after, the same blobs packed 9 ms. Storage::DataBase keeps writing a blob made in play
// as its own file (a session killed mid-write loses nothing), folds those files into a new pack
// in the background, and the next start swaps it in.
//
// Format (little-endian): a 64-byte header, the blobs, then the index: each blob's type, file
// name, place and XXH3-64 hash, then the files the pack took in (their name, size and write time,
// so the next start deletes exactly those). The header carries its own hash and the index's.
// Every count, offset and hash is checked before use and every blob against its hash when read:
// a damaged pack is not used, a damaged blob reads as missing (its shader compiles again).

#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace VideoCore::CachePack {

inline constexpr u32 Magic = 0x4B504242; // "BBPK"
inline constexpr u32 Version = 1;
inline constexpr size_t HeaderSize = 64;
inline constexpr size_t MaxName = 255;
inline constexpr u32 MaxEntries = 1u << 22;
inline constexpr u64 MaxBlob = 64ull << 20;

struct Entry {
    u8 type{};
    std::string name; // the blob's file name ("<name>.<ext>")
    u64 offset{};     // from the start of the blobs
    u64 size{};
    u64 hash{};
};

// A file the pack took in (a blob's own file, or a removal mark), as it was when read.
struct Absorbed {
    std::string name;
    u64 size{};
    s64 time{}; // its last write (std::filesystem::file_time_type ticks)
};

class Reader {
public:
    // False when the file is missing or damaged in any way (nothing of it is used then).
    bool Open(const std::filesystem::path& path);
    const Entry* Find(const std::string& name) const;
    // The blob, checked against its hash; false (and `out` empty) when it cannot be read whole or
    // does not match. Thread-safe.
    bool Read(const Entry& entry, std::vector<u8>& out) const;
    const std::vector<Entry>& Entries() const {
        return entries;
    }
    const std::vector<Absorbed>& AbsorbedFiles() const {
        return absorbed;
    }
    u64 DataBytes() const {
        return data_bytes;
    }

private:
    mutable std::ifstream file;
    mutable std::mutex mutex;
    u64 data_bytes{};
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> by_name;
    std::vector<Absorbed> absorbed;
};

class Writer {
public:
    bool Begin(const std::filesystem::path& path);
    // A name added twice keeps the first.
    bool Add(u8 type, const std::string& name, std::span<const u8> data);
    void Absorb(Absorbed file);
    // Writes the index and the header and closes the file; false when anything failed.
    bool Finish();
    size_t Count() const {
        return entries.size();
    }
    u64 Bytes() const {
        return data_bytes;
    }

private:
    std::ofstream file;
    bool ok{};
    u64 data_bytes{};
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> by_name;
    std::vector<Absorbed> absorbed;
};

} // namespace VideoCore::CachePack
