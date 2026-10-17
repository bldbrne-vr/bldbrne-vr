// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_PASS_MEMO (src/video_core/renderer_vulkan/vk_rasterizer.cpp: PassMemo,
// NoteTargetViews), its pure parts. TargetViews::Note's repeat skip (vk_pipeline_skip.h) leaves
// the table exactly as noting every draw does, in Memo and in Verify mode, over random sequences
// that repeat the last targets, change their view mask and use keys sharing a slot; Verify finds
// nothing. And the premise of the targets' repeated GetImage (the native-stereo loops' and
// BeginRendering's; FAST_CPU_LRU_TICK returns early for them), on the real
// Common::LeastRecentlyUsedCache: a touch repeated in the GC period of an earlier touch
// (PrepareRenderState's, the same draw) changes neither the LRU order nor any tick, so leaving it
// out collects the same images.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "common/lru_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_skip.h"

namespace {

using namespace Vulkan::PipelineSkip;
using NoteMemo = TargetViews::NoteMemo;

// A pool of target keys (AddTarget, as the rasterizer makes them) in which the first ones share
// one slot of the table: found as the TargetViews test does, by the slot a note takes over.
std::vector<std::uint64_t> KeyPool() {
    std::vector<std::uint64_t> keys;
    const std::uint64_t first = AddTarget(AddTarget(0, 0x6'0000'0000ull, 0), 0x6'2000'0000ull, 8);
    keys.push_back(first);
    const auto probe = std::make_unique<TargetViews>();
    for (std::uint64_t address = 0x7'0000'0000ull; keys.size() < 4; address += 0x100) {
        probe->Note(first, 3);
        const std::uint64_t candidate = AddTarget(0, address, 0);
        probe->Note(candidate, 0);
        if (probe->Find(first) == TargetViews::Unknown) {
            keys.push_back(candidate); // shares the first key's slot
        }
    }
    for (std::uint64_t i = 0; i < 28; ++i) {
        keys.push_back(AddTarget(AddTarget(0, 0x8'0000'0000ull + i * 0x40'0000ull, i % 8),
                                 0x9'0000'0000ull + (i / 3) * 0x40'0000ull, 8));
    }
    return keys;
}

TEST(PassMemo, NoteRepeatSkipKeepsTheTable) {
    const auto keys = KeyPool();
    const auto off = std::make_unique<TargetViews>();
    const auto memo = std::make_unique<TargetViews>();
    const auto verify = std::make_unique<TargetViews>();
    std::mt19937_64 rng{0x5eed'9a55};
    std::uint64_t key = keys[0];
    std::uint32_t mask = 3;
    for (int step = 0; step < 200'000; ++step) {
        const std::uint32_t roll = std::uint32_t(rng() % 100);
        if (roll < 15) {
            key = keys[rng() % keys.size()]; // another pass
        } else if (roll < 20) {
            key = keys[rng() % 4]; // a key sharing the first slot
        } else if (roll < 23) {
            mask ^= 3; // the same targets drawn with the other view mask
        } else if (roll < 24) {
            key = 0; // no attachment
        } // else: the same targets as the draw before (most draws)
        off->Note(key, mask);
        EXPECT_TRUE(memo->Note(key, mask, NoteMemo::Memo));
        EXPECT_TRUE(verify->Note(key, mask, NoteMemo::Verify));
        if (step % 997 == 0 || step < 64) {
            for (const std::uint64_t k : keys) {
                ASSERT_EQ(off->Find(k), memo->Find(k)) << "step " << step;
                ASSERT_EQ(off->Find(k), verify->Find(k)) << "step " << step;
            }
        }
    }
    for (const std::uint64_t k : keys) {
        EXPECT_EQ(off->Find(k), memo->Find(k));
        EXPECT_EQ(off->Find(k), verify->Find(k));
    }
}

TEST(PassMemo, NoteRepeatSkipOnlySkipsTheLastEntry) {
    const auto keys = KeyPool();
    const auto views = std::make_unique<TargetViews>();
    const std::uint64_t a = keys[0], b = keys[1]; // one slot
    EXPECT_TRUE(views->Note(a, 3, NoteMemo::Memo));
    EXPECT_EQ(views->Find(a), 3);
    EXPECT_TRUE(views->Note(a, 3, NoteMemo::Memo)); // a repeat: skipped, still held
    EXPECT_EQ(views->Find(a), 3);
    EXPECT_TRUE(views->Note(b, 0, NoteMemo::Memo)); // takes the slot
    EXPECT_EQ(views->Find(a), TargetViews::Unknown);
    EXPECT_TRUE(views->Note(a, 3, NoteMemo::Memo)); // not the last entry: stored again
    EXPECT_EQ(views->Find(a), 3);
    EXPECT_EQ(views->Find(b), TargetViews::Unknown);
    EXPECT_TRUE(views->Note(a, 0, NoteMemo::Memo)); // another view mask is another entry
    EXPECT_EQ(views->Find(a), 0);
    EXPECT_TRUE(views->Note(0, 3, NoteMemo::Memo)); // key 0 is never kept and changes nothing
    EXPECT_TRUE(views->Note(a, 0, NoteMemo::Memo));
    EXPECT_EQ(views->Find(a), 0);
    // A note without the memo in between keeps the last entry too (every mode does), so a mode
    // change mid-run stays exact.
    EXPECT_TRUE(views->Note(b, 3));
    EXPECT_TRUE(views->Note(a, 0, NoteMemo::Memo));
    EXPECT_EQ(views->Find(a), 0);
    EXPECT_TRUE(views->Note(a, 0, NoteMemo::Verify));
}

// The targets' repeated GetImage: a frame of draws, each touching its targets first
// (PrepareRenderState's GetImage) and its textures, then its targets again (the GetImage calls of
// the native-stereo loop and BeginRendering) in one cache and not in the other; a collection ends
// each GC period.
TEST(PassMemo, RepeatedTouchInThePeriodChangesNothing) {
    using Lru = Common::LeastRecentlyUsedCache<std::uint32_t, std::uint64_t>;
    constexpr std::uint32_t Images = 96;
    Lru touched, peeked;
    std::vector<std::size_t> touched_ids(Images), peeked_ids(Images);
    std::vector<bool> alive(Images, true);
    std::uint64_t gc_tick = 0;
    for (std::uint32_t i = 0; i < Images; ++i) {
        touched_ids[i] = touched.Insert(i, gc_tick);
        peeked_ids[i] = peeked.Insert(i, gc_tick);
    }
    const auto order = [](Lru& lru) {
        std::vector<std::uint32_t> result;
        lru.ForEachItemBelow(~std::uint64_t{0} >> 2,
                             [&](std::uint32_t id) { result.push_back(id); });
        return result;
    };
    std::mt19937_64 rng{0x9a55'3e30};
    for (int period = 0; period < 400; ++period) {
        const int draws = 20 + int(rng() % 200);
        for (int draw = 0; draw < draws; ++draw) {
            std::vector<std::uint32_t> targets;
            for (std::uint32_t t = 1 + std::uint32_t(rng() % 5); t > 0; --t) {
                const std::uint32_t id = std::uint32_t(rng() % Images);
                if (alive[id]) {
                    targets.push_back(id);
                }
            }
            for (const std::uint32_t id : targets) { // PrepareRenderState
                touched.Touch(touched_ids[id], gc_tick);
                peeked.Touch(peeked_ids[id], gc_tick);
            }
            for (std::uint32_t t = std::uint32_t(rng() % 6); t > 0; --t) { // textures
                const std::uint32_t id = std::uint32_t(rng() % Images);
                if (alive[id]) {
                    touched.Touch(touched_ids[id], gc_tick);
                    peeked.Touch(peeked_ids[id], gc_tick);
                }
            }
            for (const std::uint32_t id : targets) { // the repeats FAST_CPU_LRU_TICK cuts short
                touched.Touch(touched_ids[id], gc_tick);
            }
        }
        // RunGarbageCollector: the same images below the age, freed, then a new period; a freed
        // image's slot is registered again later (Insert at gc_tick).
        std::vector<std::uint32_t> collected_touched, collected_peeked;
        if (gc_tick >= 3) {
            touched.ForEachItemBelow(gc_tick - 3,
                                     [&](std::uint32_t id) { collected_touched.push_back(id); });
            peeked.ForEachItemBelow(gc_tick - 3,
                                    [&](std::uint32_t id) { collected_peeked.push_back(id); });
        }
        ASSERT_EQ(collected_touched, collected_peeked) << "period " << period;
        for (const std::uint32_t id : collected_touched) {
            touched.Free(touched_ids[id]);
            peeked.Free(peeked_ids[id]);
            alive[id] = false;
        }
        ++gc_tick;
        for (std::uint32_t id = 0; id < Images; ++id) {
            if (!alive[id] && rng() % 4 == 0) {
                touched_ids[id] = touched.Insert(id, gc_tick);
                peeked_ids[id] = peeked.Insert(id, gc_tick);
                alive[id] = true;
            }
        }
        ASSERT_EQ(order(touched), order(peeked)) << "period " << period;
        for (std::uint32_t id = 0; id < Images; ++id) {
            if (alive[id]) {
                ASSERT_EQ(touched.TickOf(touched_ids[id]), peeked.TickOf(peeked_ids[id]));
            }
        }
    }
}

} // namespace
