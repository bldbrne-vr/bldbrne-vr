// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_LRU_TICK (TextureCache::GetImage and GetImageTouch, src/video_core/texture_cache/): a
// model of the texture cache's use of the real Common::LeastRecentlyUsedCache runs one random
// sequence of registrations (Insert at gc_tick), unregistrations (Free), recycled image slots,
// GetImage and TouchImageOnce touches and garbage collections (ForEachItemBelow, freeing the
// oldest, then ++gc_tick) twice: touching on every GetImage (the old way, or the switch off) and
// skipping a GetImage whose image-local tick is gc_tick. Both must keep the same LRU order and
// ticks and collect the same images, every skip must be one the VERIFY check accepts (its entry
// at gc_tick or above), and that check must catch a tick set without a touch.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/lru_cache.h"

namespace {

using Tick = std::uint64_t;
constexpr Tick Never = ~Tick{0};
constexpr std::uint32_t Slots = 48;

struct Image {
    std::size_t lru_id{};
    Tick lru_touched_tick{Never}; // a new Image in a recycled slot starts here
    bool alive{};                 // its slot holds an image
    bool registered{};
};

struct Cache {
    explicit Cache(bool skip_) : skip{skip_} {}

    bool skip; // FAST_CPU_LRU_TICK
    Common::LeastRecentlyUsedCache<std::uint32_t, Tick> lru;
    std::vector<Image> images = std::vector<Image>(Slots);
    Tick gc_tick = 0;
    std::uint64_t touches = 0, skipped = 0, verify_mismatches = 0;

    void Create(std::uint32_t id) {
        images[id] = Image{};
        images[id].alive = true;
    }
    // TextureCache::RegisterImage / UnregisterImage (their LRU part).
    void Register(std::uint32_t id) {
        auto& image = images[id];
        image.registered = true;
        image.lru_id = lru.Insert(id, gc_tick);
    }
    void Unregister(std::uint32_t id) {
        auto& image = images[id];
        image.registered = false;
        lru.Free(image.lru_id);
    }
    // TextureCache::GetImage and GetImageTouch.
    void GetImage(std::uint32_t id) {
        auto& image = images[id];
        if (skip && image.lru_touched_tick == gc_tick) {
            ++skipped;
            if (lru.TickOf(image.lru_id) < gc_tick) {
                ++verify_mismatches; // what FAST_CPU_LRU_TICK_VERIFY reports
            }
            return;
        }
        ++touches;
        lru.Touch(image.lru_id, gc_tick);
        if (skip) {
            image.lru_touched_tick = gc_tick;
        }
    }
    // TextureCache::TouchImageOnce (FAST bit 26), as it is in both runs.
    void TouchImageOnce(std::uint32_t id) {
        auto& image = images[id];
        if (image.lru_touched_tick == gc_tick || !image.registered) {
            return;
        }
        lru.Touch(image.lru_id, gc_tick);
        image.lru_touched_tick = gc_tick;
    }
    // TextureCache::RunGarbageCollector: GarbageCollectImages frees up to `count` images not
    // touched for `age` periods (FreeImage), then the period ends.
    std::vector<std::uint32_t> Collect(Tick age, int count) {
        std::vector<std::uint32_t> freed;
        const Tick ticks = std::min(age, gc_tick);
        lru.ForEachItemBelow(gc_tick - ticks, [&](std::uint32_t id) {
            if (count == 0) {
                return true;
            }
            --count;
            Unregister(id);
            images[id].alive = false;
            freed.push_back(id);
            return false;
        });
        ++gc_tick;
        return freed;
    }
    // The LRU list from the oldest entry: its objects and their ticks.
    std::vector<std::pair<std::uint32_t, Tick>> Order() {
        std::vector<std::pair<std::uint32_t, Tick>> order;
        lru.ForEachItemBelow(gc_tick, [&](std::uint32_t id) {
            order.emplace_back(id, lru.TickOf(images[id].lru_id));
        });
        return order;
    }
};

template <typename Pred>
int Pick(std::mt19937& rng, const Cache& cache, Pred&& pred) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t id = 0; id < Slots; ++id) {
        if (pred(cache.images[id])) {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return -1;
    }
    return int(ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)]);
}

TEST(LruTick, SkipsOnlyTouchesThatChangeNothing) {
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        Cache old_way(false), skipping(true);
        const auto both = [&](auto&& op) {
            op(old_way);
            op(skipping);
        };
        // About 100 steps per GC period, as a frame's many GetImage calls share one.
        std::uniform_int_distribution<int> permille(0, 999);
        for (int step = 0; step < 8000; ++step) {
            const int r = permille(rng);
            if (r < 640) {
                const int id = Pick(rng, skipping, [](const Image& i) { return i.registered; });
                if (id >= 0) {
                    both([&](Cache& c) { c.GetImage(u32(id)); });
                }
            } else if (r < 720) {
                const int id = Pick(rng, skipping, [](const Image& i) { return i.alive; });
                if (id >= 0) {
                    both([&](Cache& c) { c.TouchImageOnce(u32(id)); });
                }
            } else if (r < 790) {
                // A new image, often in a recycled slot.
                const int id = Pick(rng, skipping, [](const Image& i) { return !i.alive; });
                if (id >= 0) {
                    both([&](Cache& c) {
                        c.Create(u32(id));
                        c.Register(u32(id));
                    });
                }
            } else if (r < 830) {
                // FreeImage.
                const int id = Pick(rng, skipping, [](const Image& i) { return i.registered; });
                if (id >= 0) {
                    both([&](Cache& c) {
                        c.Unregister(u32(id));
                        c.images[id].alive = false;
                    });
                }
            } else if (r < 860) {
                // Unregistered, the image itself kept (and maybe registered again later).
                const int id = Pick(rng, skipping, [](const Image& i) { return i.registered; });
                if (id >= 0) {
                    both([&](Cache& c) { c.Unregister(u32(id)); });
                }
            } else if (r < 890) {
                const int id = Pick(rng, skipping,
                                    [](const Image& i) { return i.alive && !i.registered; });
                if (id >= 0) {
                    both([&](Cache& c) { c.Register(u32(id)); });
                }
            } else if (r < 990) {
                // GetImage of an unregistered image the tick skips: the old way touches its
                // freed (perhaps reused) entry, which must change nothing either.
                const int id = Pick(rng, skipping, [&](const Image& i) {
                    return i.alive && !i.registered && i.lru_touched_tick == skipping.gc_tick;
                });
                if (id >= 0) {
                    both([&](Cache& c) { c.GetImage(u32(id)); });
                }
            } else if (r < 996) {
                const Tick age = std::uniform_int_distribution<Tick>(0, 3)(rng);
                const int count = std::uniform_int_distribution<int>(0, 4)(rng);
                EXPECT_EQ(old_way.Collect(age, count), skipping.Collect(age, count));
            } else {
                // Below the GC trigger: only the period ends.
                both([](Cache& c) { ++c.gc_tick; });
            }
            ASSERT_EQ(old_way.Order(), skipping.Order()) << "seed " << seed << " step " << step;
        }
        EXPECT_EQ(skipping.verify_mismatches, 0u);
        EXPECT_EQ(old_way.skipped, 0u);
        // Many GetImage calls repeat an image of the period.
        EXPECT_GT(skipping.skipped, 1000u) << "seed " << seed;
        EXPECT_EQ(skipping.skipped + skipping.touches, old_way.touches);
    }
}

TEST(LruTick, VerifyCatchesATickWithoutATouch) {
    Cache cache(true);
    cache.Create(0);
    cache.Register(0);
    cache.Create(1);
    cache.Register(1);
    ++cache.gc_tick;
    cache.GetImage(1); // a real touch: entry 1 moves past entry 0
    cache.images[0].lru_touched_tick = cache.gc_tick; // a tick set without a touch
    cache.GetImage(0);
    EXPECT_EQ(cache.skipped, 1u);
    EXPECT_EQ(cache.verify_mismatches, 1u);
    // The old way would have moved image 0 to the end; the forged skip left it first.
    Cache old_way(false);
    old_way.Create(0);
    old_way.Register(0);
    old_way.Create(1);
    old_way.Register(1);
    ++old_way.gc_tick;
    old_way.GetImage(1);
    old_way.GetImage(0);
    EXPECT_NE(old_way.Order(), cache.Order());
}

TEST(LruTick, SecondRegistrationInAPeriodInsertsAtItsTick) {
    // An image touched, unregistered and registered again in one period: its new entry was
    // inserted at gc_tick, so the skip of its next GetImage is still exact.
    Cache cache(true);
    cache.Create(0);
    cache.Register(0);
    cache.Create(1);
    cache.Register(1);
    ++cache.gc_tick;
    cache.GetImage(0);
    cache.Unregister(0);
    cache.Create(2);
    cache.Register(2); // reuses image 0's freed entry
    cache.Register(0); // a new entry for image 0
    cache.GetImage(0);
    EXPECT_EQ(cache.skipped, 1u);
    EXPECT_EQ(cache.verify_mismatches, 0u);
    EXPECT_LE(cache.gc_tick, cache.lru.TickOf(cache.images[0].lru_id));
}

} // namespace
