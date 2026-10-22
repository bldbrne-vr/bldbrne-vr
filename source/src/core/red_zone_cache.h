// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>
#include <windows.h>

#include "common/sha1.h"

namespace Core {

// Only analysis decisions are persisted. Code and trampolines are always generated afresh.
// This is a disposable, local cache, not an executable patch format.
class RedZoneAnalysisCache {
public:
    enum Requirement : uint8_t { Always = 1, WithoutSSE4a = 2, DirectCpu = 4 };

    RedZoneAnalysisCache(const std::filesystem::path& directory, uintptr_t segment, size_t size,
                         std::span<const uintptr_t> starts)
        : records(starts.size()) {
#ifdef RED_ZONE_CACHE_BUILD_ID
        if (directory.empty()) {
            return;
        }
        sha1::SHA1 hash;
        constexpr char version[] = "BBVR red-zone analysis v3:" RED_ZONE_CACHE_BUILD_ID;
        const auto decoder_version = ZydisGetVersion();
        hash.processBytes(version, sizeof(version));
        hash.processBytes(&decoder_version, sizeof(decoder_version));
        hash.processBytes(&size, sizeof(size));
        hash.processBytes(reinterpret_cast<const void*>(segment), size);
        for (const auto start : starts) {
            const auto offset = start - segment;
            hash.processBytes(&offset, sizeof(offset));
        }
        hash.getDigestBytes(key.data());
        std::string filename;
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : key) {
            filename += hex[byte >> 4];
            filename += hex[byte & 15];
        }
        path = directory / (filename + ".bin");
        std::error_code error;
        const auto base_size =
            key.size() * 2 + records.size() * sizeof(records[0]) + sizeof(uint64_t);
        const auto file_size = std::filesystem::file_size(path, error);
        if (error || file_size < base_size) {
            return;
        }
        std::ifstream input(path, std::ios::binary);
        std::array<uint8_t, 20> stored_key{}, checksum{};
        input.read(reinterpret_cast<char*>(stored_key.data()), stored_key.size());
        input.read(reinterpret_cast<char*>(checksum.data()), checksum.size());
        input.read(reinterpret_cast<char*>(records.data()), records.size() * sizeof(records[0]));
        uint64_t site_count{};
        input.read(reinterpret_cast<char*>(&site_count), sizeof(site_count));
        if (!input || site_count > size / 5 || file_size != base_size + site_count * sizeof(Site)) {
            return;
        }
        sites.resize(site_count);
        if (!sites.empty()) {
            input.read(reinterpret_cast<char*>(sites.data()), sites.size() * sizeof(Site));
        }
        if (!input || stored_key != key || checksum != Checksum()) {
            sites.clear();
            return;
        }
        // Counts and cached instruction offsets are bounded by the current unwind table.
        // The executable hash covers instruction bytes; absolute addresses are regenerated.
        for (size_t i = 0; i < starts.size(); ++i) {
            const auto end = i + 1 < starts.size() ? starts[i + 1] : segment + size;
            if ((records[i] >> 3) > end - starts[i]) {
                return;
            }
        }
        size_t next_site = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            const size_t begin = next_site;
            const auto end = i + 1 < starts.size() ? starts[i + 1] : segment + size;
            uintptr_t previous_end = starts[i] - segment;
            while (next_site < sites.size() && sites[next_site][0] == i) {
                const auto offset = sites[next_site++][1];
                if (offset < previous_end || offset >= end - segment ||
                    end - segment - offset < 5) {
                    sites.clear();
                    return;
                }
                previous_end = offset + 5;
            }
            if (bool(records[i] & DirectCpu) != (begin != next_site) ||
                ((records[i] & DirectCpu) && !(records[i] & (Always | WithoutSSE4a)))) {
                sites.clear();
                return;
            }
        }
        if (next_site != sites.size()) {
            sites.clear();
            return;
        }
        hit = true;
#endif
    }

    bool Hit() const {
        return hit;
    }
    bool NeedsAnalysis(size_t index, bool needs_sse4a_substitutions) const {
        const uint64_t mask = Always | (needs_sse4a_substitutions ? WithoutSSE4a : 0);
        return (records[index] & mask) != 0;
    }
    uint64_t InstructionCount(size_t index) const {
        return records[index] >> 3;
    }
    void Record(size_t index, size_t instruction_count, uint8_t requirements) {
        if (!hit && !rebuilding) {
            sites.clear();
            rebuilding = true;
        }
        records[index] = (uint64_t(instruction_count) << 3) | requirements;
    }

    bool CanPatchDirectly(size_t index) const {
        return (records[index] & DirectCpu) != 0;
    }
    using Site = std::array<uint64_t, 2>; // Function index, executable-relative instruction offset.
    void AddDirectSite(size_t index, uintptr_t offset) {
        sites.push_back({index, offset});
    }
    std::span<const Site> DirectSites(size_t index) const {
        const auto first =
            std::lower_bound(sites.begin(), sites.end(), index,
                             [](const Site& site, size_t function) { return site[0] < function; });
        const auto last =
            std::upper_bound(first, sites.end(), index,
                             [](size_t function, const Site& site) { return function < site[0]; });
        return {first, last};
    }

    bool Save() const {
        if (hit || path.empty()) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
        // Distinct writers cannot expose a half-written cache. A crash or corrupt file just
        // causes a full analysis next time; cache I/O never changes protection eligibility.
        static std::atomic<uint64_t> sequence{};
        auto temporary = path;
        temporary += "." + std::to_string(GetCurrentProcessId()) + "." +
                     std::to_string(sequence.fetch_add(1)) + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto checksum = Checksum();
        output.write(reinterpret_cast<const char*>(key.data()), key.size());
        output.write(reinterpret_cast<const char*>(checksum.data()), checksum.size());
        output.write(reinterpret_cast<const char*>(records.data()),
                     records.size() * sizeof(records[0]));
        const uint64_t site_count = sites.size();
        output.write(reinterpret_cast<const char*>(&site_count), sizeof(site_count));
        if (!sites.empty()) {
            output.write(reinterpret_cast<const char*>(sites.data()), sites.size() * sizeof(Site));
        }
        output.close();
        const bool saved =
            output && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!saved) {
            std::filesystem::remove(temporary, error);
        }
        return saved;
    }

private:
    std::array<uint8_t, 20> Checksum() const {
        sha1::SHA1 hash;
        hash.processBytes(key.data(), key.size());
        if (!records.empty()) {
            hash.processBytes(records.data(), records.size() * sizeof(records[0]));
        }
        if (!sites.empty()) {
            hash.processBytes(sites.data(), sites.size() * sizeof(Site));
        }
        std::array<uint8_t, 20> digest{};
        hash.getDigestBytes(digest.data());
        return digest;
    }

    std::filesystem::path path;
    std::array<uint8_t, 20> key{};
    std::vector<uint64_t> records;
    std::vector<Site> sites;
    bool hit{};
    bool rebuilding{};
};

} // namespace Core
