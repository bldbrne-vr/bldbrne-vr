// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_SPARSE_FAST's area cache (scratchpad/dual-eye-perf/sparse_range_cache.h, used by
// Core::MemoryManager::CopySparseMemory and TryWriteBacking): the area a thread looked up last
// answers ranges in any slot, a slot hit becomes the recent area, the bounds and the overflow are
// checked as Find checks them, every exclusive release but UnlockKeepingRanges forgets every area,
// and a model of the copy (areas split, merged, mapped and unmapped under the lock, guest fence
// writes that keep the areas in between) copies through the cache exactly the bytes the tree walk
// copies. Concurrent readers never see an area older than the last release that changed one.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "scratchpad/dual-eye-perf/sparse_range_cache.h"

namespace {

using namespace ScratchSparse;

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;

TEST(SparseFast, RecentAreaAnswersEverySlot) {
    MappingMutex mutex;
    RangeCache cache;
    std::shared_lock lock(mutex);
    // A 16 MiB area: its 64 KiB granules fall in every one of the 64 slots.
    cache.RememberRecent(mutex, 0x100000, 0x100000, 16 * MiB, true);
    for (std::uint64_t offset = 0; offset < 16 * MiB; offset += 64 * KiB) {
        const auto* hit = cache.FindRecent(mutex, 0x100000 + offset, 256);
        ASSERT_NE(hit, nullptr) << offset;
        EXPECT_EQ(hit->base, 0x100000u);
        EXPECT_EQ(hit->size, 16 * MiB);
        EXPECT_TRUE(hit->mapped);
    }
    // The slot table alone knows only the remembered address's slot.
    EXPECT_NE(cache.Find(mutex, 0x100000, 1), nullptr);
    EXPECT_EQ(cache.Find(mutex, 0x100000 + 64 * KiB, 1), nullptr);
}

TEST(SparseFast, ASlotHitBecomesTheRecentArea) {
    MappingMutex mutex;
    RangeCache cache;
    std::shared_lock lock(mutex);
    cache.RememberRecent(mutex, 0x100000, 0x100000, 4 * MiB, true);         // slot 16
    cache.RememberRecent(mutex, 0x10000000, 0x10000000, 64 * KiB, false);   // slot 0, recent
    // 0x110000 is in the first area but another slot: neither the recent area nor its slot.
    EXPECT_EQ(cache.FindRecent(mutex, 0x110000, 16), nullptr);
    // Its remembered slot hits and the first area becomes the recent one again.
    const auto* hit = cache.FindRecent(mutex, 0x100000, 16);
    ASSERT_NE(hit, nullptr);
    EXPECT_TRUE(hit->mapped);
    hit = cache.FindRecent(mutex, 0x110000, 16);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->base, 0x100000u);
    // The second area still answers from its slot (unmapped: read as zeros).
    hit = cache.FindRecent(mutex, 0x10000000, 64 * KiB);
    ASSERT_NE(hit, nullptr);
    EXPECT_FALSE(hit->mapped);
}

TEST(SparseFast, BoundsAndOverflowAsFind) {
    MappingMutex mutex;
    RangeCache cache;
    std::shared_lock lock(mutex);
    cache.RememberRecent(mutex, 100, 100, 100, true);
    EXPECT_NE(cache.FindRecent(mutex, 100, 100), nullptr);
    EXPECT_NE(cache.FindRecent(mutex, 199, 1), nullptr);
    EXPECT_EQ(cache.FindRecent(mutex, 99, 1), nullptr);
    EXPECT_EQ(cache.FindRecent(mutex, 199, 2), nullptr);
    EXPECT_EQ(cache.FindRecent(mutex, 200, 0), nullptr);
    EXPECT_EQ(cache.FindRecent(mutex, 100, UINT64_MAX), nullptr);
    EXPECT_EQ(cache.FindRecent(mutex, 150, UINT64_MAX - 10), nullptr);
    // Find and FindRecent agree wherever the slot is the remembered one.
    for (std::uint64_t address = 90; address < 210; ++address) {
        for (std::uint64_t size : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{50},
                                   std::uint64_t{100}, UINT64_MAX}) {
            EXPECT_EQ(cache.Find(mutex, address, size) != nullptr,
                      cache.FindRecent(mutex, address, size) != nullptr)
                << address << " " << size;
        }
    }
}

TEST(SparseFast, OnlyAKeepingReleaseKeepsTheAreas) {
    MappingMutex mutex;
    RangeCache cache;
    {
        std::shared_lock lock(mutex);
        cache.RememberRecent(mutex, 0x100000, 0x100000, MiB, true);
    }
    // A guest fence write (TryWriteBacking): exclusive, no area changed.
    mutex.lock();
    mutex.UnlockKeepingRanges();
    {
        std::shared_lock lock(mutex);
        EXPECT_NE(cache.FindRecent(mutex, 0x180000, 64), nullptr);
        EXPECT_NE(cache.Find(mutex, 0x100000, 64), nullptr);
    }
    // Any other exclusive release (a map, an unmap, a protection change) forgets every area.
    mutex.lock();
    mutex.unlock();
    {
        std::shared_lock lock(mutex);
        EXPECT_EQ(cache.FindRecent(mutex, 0x180000, 64), nullptr);
        EXPECT_EQ(cache.Find(mutex, 0x100000, 64), nullptr);
        cache.RememberRecent(mutex, 0x100000, 0x100000, MiB, false);
        EXPECT_NE(cache.FindRecent(mutex, 0x100000, 64), nullptr);
    }
    // std::scoped_lock's failed attempts release with unlock(): forgotten too.
    std::mutex other;
    other.lock();
    std::thread([&] {
        std::unique_lock first(mutex, std::defer_lock);
        std::unique_lock second(other, std::defer_lock);
        EXPECT_EQ(std::try_lock(first, second), 1);
    }).join();
    other.unlock();
    std::shared_lock lock(mutex);
    EXPECT_EQ(cache.FindRecent(mutex, 0x100000, 64), nullptr);
    // Another mutex (another memory manager) never matches.
    MappingMutex second_manager;
    std::shared_lock other_lock(second_manager);
    cache.RememberRecent(mutex, 0x100000, 0x100000, MiB, true);
    EXPECT_EQ(cache.FindRecent(second_manager, 0x100000, 64), nullptr);
}

// A model of the guest address space: areas keyed by base (the memory manager's vma_map), a byte
// image, and the two copies: the tree walk (CopySparseMemory before FAST_CPU_SPARSE_FAST) and the
// fast path (the cached area, else the tree walk that remembers it).
struct Area {
    std::uint64_t base, size;
    bool mapped;
};

struct Model {
    static constexpr std::uint64_t Lower = 0x10000;
    static constexpr std::uint64_t Span = 8 * MiB;
    std::map<std::uint64_t, Area> areas;
    std::vector<std::uint8_t> bytes;

    Model() : bytes(Span) {
        areas[Lower] = {Lower, Span, true};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(i * 2654435761u >> 13);
        }
    }
    auto Find(std::uint64_t address) {
        return std::prev(areas.upper_bound(address));
    }
    void Walk(std::map<std::uint64_t, Area>::iterator area, std::uint64_t address,
              std::uint8_t* dest, std::uint64_t size) const {
        while (size) {
            const std::uint64_t n =
                std::min<std::uint64_t>(area->second.size - (address - area->first), size);
            if (area->second.mapped) {
                std::memcpy(dest, bytes.data() + (address - Lower), n);
            } else {
                std::memset(dest, 0, n);
            }
            size -= n;
            address += n;
            dest += n;
            ++area;
        }
    }
    // Split at `at` (inside an area), as CarveVMA's Split.
    void Split(std::uint64_t at) {
        auto area = Find(at);
        if (area->first == at) {
            return;
        }
        Area tail{at, area->second.base + area->second.size - at, area->second.mapped};
        area->second.size = at - area->second.base;
        areas[at] = tail;
    }
    // Map or unmap [begin, end) and merge equal neighbours, as MapMemory/UnmapMemory with
    // MergeAdjacent.
    void SetMapped(std::uint64_t begin, std::uint64_t end, bool mapped) {
        Split(begin);
        if (end < Lower + Span) {
            Split(end);
        }
        for (auto area = Find(begin); area != areas.end() && area->first < end; ++area) {
            area->second.mapped = mapped;
        }
        for (auto area = areas.begin(); std::next(area) != areas.end();) {
            auto next = std::next(area);
            if (area->second.mapped == next->second.mapped) {
                area->second.size += next->second.size;
                areas.erase(next);
            } else {
                ++area;
            }
        }
    }
};

TEST(SparseFast, CopiesTheTreeWalksBytesThroughEveryChange) {
    std::mt19937_64 rng(20261014);
    MappingMutex mutex;
    RangeCache cache;
    Model model;
    std::vector<std::uint8_t> want(512 * KiB), got(512 * KiB);
    std::uint64_t hits = 0, kept_hits = 0, copies = 0, previous = Model::Lower;
    bool after_keep = false;
    for (int step = 0; step < 20000; ++step) {
        const auto pick = rng() % 100;
        if (pick < 3) {
            // A mapping change: the exclusive release forgets every area.
            std::unique_lock lock(mutex);
            const std::uint64_t begin = Model::Lower + (rng() % (Model::Span / (4 * KiB))) * 4 * KiB;
            const std::uint64_t end =
                std::min(Model::Lower + Model::Span, begin + (1 + rng() % 64) * 16 * KiB);
            model.SetMapped(begin, end, rng() % 2 == 0);
            after_keep = false;
        } else if (pick < 10) {
            // A guest fence write through the backing: bytes change, no area does.
            mutex.lock();
            const std::uint64_t at = rng() % (Model::Span - 8);
            const std::uint64_t value = rng();
            std::memcpy(model.bytes.data() + at, &value, 8);
            mutex.UnlockKeepingRanges();
            after_keep = true;
        } else {
            std::shared_lock lock(mutex);
            const std::uint64_t size = 1 + rng() % (rng() % 4 == 0 ? 512 * KiB : 16 * KiB);
            // Mostly near the copy before (a draw's buffers), now and then anywhere.
            std::uint64_t address = rng() % 4 == 0 ? Model::Lower + rng() % Model::Span
                                                   : previous + rng() % (64 * KiB);
            address = std::min(address, Model::Lower + Model::Span - size);
            previous = address;
            model.Walk(model.Find(address), address, want.data(), size);
            if (const auto* hit = cache.FindRecent(mutex, address, size)) {
                ++hits;
                kept_hits += after_keep;
                if (hit->mapped) {
                    std::memcpy(got.data(), model.bytes.data() + (address - Model::Lower), size);
                } else {
                    std::memset(got.data(), 0, size);
                }
            } else {
                const auto area = model.Find(address);
                cache.RememberRecent(mutex, address, area->second.base, area->second.size,
                                     area->second.mapped);
                model.Walk(area, address, got.data(), size);
            }
            ++copies;
            after_keep = false;
            ASSERT_EQ(std::memcmp(want.data(), got.data(), size), 0)
                << "step " << step << " address " << address << " size " << size;
        }
    }
    // The cache answers, also right after a fence write (which keeps it).
    EXPECT_GT(hits, copies / 4);
    EXPECT_GT(kept_hits, 0u);
}

TEST(SparseFast, ReadersNeverSeeAnAreaOlderThanTheLastChange) {
    MappingMutex mutex;
    std::uint64_t size = 1 * MiB; // the area's truth, changed only under the exclusive lock
    bool mapped = true;
    std::atomic<int> finished{0};
    std::atomic<std::uint64_t> hits{0}, stale{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            RangeCache local;
            std::mt19937_64 rng(r);
            for (int j = 0; j < 200000; ++j) {
                {
                    std::shared_lock lock(mutex);
                    const std::uint64_t address = 0x100000 + rng() % (512 * KiB);
                    if (const auto* hit = local.FindRecent(mutex, address, 64)) {
                        stale.fetch_add(hit->size != size || hit->mapped != mapped,
                                        std::memory_order_relaxed);
                        hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        local.RememberRecent(mutex, address, 0x100000, size, mapped);
                    }
                }
                // Readers come first (SharedFirstMutex): leave the writer a gap now and then.
                if ((j & 7) == 0) {
                    std::this_thread::yield();
                }
            }
            finished.fetch_add(1);
        });
    }
    std::mt19937_64 rng(99);
    std::uint64_t changes = 0, keeps = 0;
    while (finished.load() < 4) {
        mutex.lock();
        if (rng() % 4 == 0) {
            size = size == 1 * MiB ? 2 * MiB : 1 * MiB;
            mapped = !mapped;
            ++changes;
            mutex.unlock();
        } else {
            ++keeps;
            mutex.UnlockKeepingRanges(); // a fence write: the truth is unchanged
        }
    }
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(stale.load(), 0u);
    EXPECT_GT(hits.load(), 0u);
    EXPECT_GT(changes, 0u);
    EXPECT_GT(keeps, 0u);
}

} // namespace
