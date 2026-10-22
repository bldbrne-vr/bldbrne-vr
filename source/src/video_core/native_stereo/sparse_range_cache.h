// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include "common/shared_first_mutex.h"

namespace ScratchSparse {
// Same lock semantics as the memory manager's original mutex. All exclusive
// releases invalidate cached mapping metadata, including early unlock/relock,
// except UnlockKeepingRanges after a hold that changed none.
class MappingMutex : public Common::SharedFirstMutex {
public:
    void unlock() {
        ++generation;
        Common::SharedFirstMutex::unlock();
    }
    // FAST_CPU_SPARSE_FAST: the exclusive release of a hold that changed no mapping metadata (no
    // area added, removed, split, merged or retyped): every cached range stays exact, so it
    // stays valid. Any other exclusive release must use unlock().
    void UnlockKeepingRanges() { Common::SharedFirstMutex::unlock(); }
    // Caller must hold this mutex shared or exclusively.
    std::uint64_t Generation() const { return generation; }
    std::uint64_t Identity() const { return identity; }
private:
    inline static std::atomic<std::uint64_t> next_identity{1};
    const std::uint64_t identity = next_identity.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t generation{};
};

class RangeCache {
public:
    struct Entry {
        std::uint64_t identity{}, generation{}, base{}, size{};
        bool mapped{};
    };
    const Entry* Find(const MappingMutex& mutex, std::uint64_t address,
                      std::uint64_t size) const {
        const auto& e = entries[Slot(address)];
        return Covers(e, mutex, address, size) ? &e : nullptr;
    }
    void Remember(const MappingMutex& mutex, std::uint64_t address,
                  std::uint64_t base, std::uint64_t size, bool mapped) {
        entries[Slot(address)] = {mutex.Identity(), mutex.Generation(), base, size, mapped};
    }
    // FAST_CPU_SPARSE_FAST: the area of the last lookup first (bbport's RegionCache: nearly
    // every copy falls in the area of the one before, whatever its 64 KiB slot), then the
    // address's slot, whose hit becomes the recent area. Same validity rules as Find.
    const Entry* FindRecent(const MappingMutex& mutex, std::uint64_t address,
                            std::uint64_t size) {
        if (Covers(recent, mutex, address, size)) return &recent;
        const auto& e = entries[Slot(address)];
        if (!Covers(e, mutex, address, size)) return nullptr;
        recent = e;
        return &recent;
    }
    void RememberRecent(const MappingMutex& mutex, std::uint64_t address,
                        std::uint64_t base, std::uint64_t size, bool mapped) {
        Remember(mutex, address, base, size, mapped);
        recent = entries[Slot(address)];
    }
private:
    // The entry was read under this mutex's current generation and its area holds the range
    // (no overflow: the offset is checked before the size).
    static bool Covers(const Entry& e, const MappingMutex& mutex, std::uint64_t address,
                       std::uint64_t size) {
        if (e.identity != mutex.Identity() || e.generation != mutex.Generation() ||
            address < e.base) return false;
        const auto offset = address - e.base;
        return offset < e.size && size <= e.size - offset;
    }
    static std::size_t Slot(std::uint64_t address) { return (address >> 16) & 63; }
    std::array<Entry, 64> entries{};
    Entry recent{};
};
}
