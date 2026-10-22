// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace ScratchUpload {
// Owned by one stream buffer, accessed only by its recording thread.
// Content is compared exactly; never infer freshness from the guest address.
struct Memo {
    static constexpr size_t MaxBytes = 4096, Slots = 64;
    struct Key {
        uint64_t address, tick, generation, alignment;
        size_t size;
        bool operator==(const Key&) const = default;
    };
    struct Entry {
        Key key{};
        uint64_t offset{};
        bool valid{};
        std::array<uint8_t, MaxBytes> bytes;
    };
    std::array<Entry, Slots> entries;
    std::array<uint8_t, MaxBytes> snapshot;
    uint64_t calls{}, hits{};
    static size_t Index(const Key& k) { return ((k.address >> 4) ^ (k.address >> 12) ^ k.size) % Slots; }
    bool MayMatch(const Key& key) const {
        const auto& e = entries[Index(key)];
        return key.size <= MaxBytes && e.valid && e.key == key;
    }
    std::optional<uint64_t> Find(const Key& key, std::span<const uint8_t> data) {
        ++calls;
        const auto& e = entries[Index(key)];
        if (data.size() <= MaxBytes && data.size() == key.size && e.valid && e.key == key &&
            std::memcmp(e.bytes.data(), data.data(), data.size()) == 0) {
            ++hits;
            return e.offset;
        }
        return {};
    }
    void Store(const Key& key, std::span<const uint8_t> data, uint64_t offset) {
        if (data.size() > MaxBytes || data.size() != key.size) return;
        auto& e = entries[Index(key)];
        e.key = key;
        e.offset = offset;
        std::memcpy(e.bytes.data(), data.data(), data.size());
        e.valid = true;
    }
};
}
