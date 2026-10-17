// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_FAULT_WINDOW (src/video_core/buffer_cache/region_definitions.h, used by
// RegionManager::MarkFaultWindow): a model of one region's CPU tracking (ChangeRegionState<CPU,
// true> and UpdateProtection) runs a guest write fault the previous way (the page's own
// invalidation, then MarkCleanPagesCpuModified's runs) and the new way (the window's pages in one
// change, then the page), and both must end with the same cpu and writeable bits and lift the same
// page watches, the new way in at most one change. The verify comparison finds no mismatch for any
// state and mode, and finds a window that marks a page too many or too few.

#include <cstddef>
#include <cstdint>
#include <random>

#include <gtest/gtest.h>

#include "video_core/buffer_cache/region_definitions.h"

namespace {

using namespace VideoCore;

constexpr size_t WindowPages = 16; // the default 64 KiB window

std::uint64_t Pages(const RegionBits& bits) {
    std::uint64_t n = 0;
    for (const auto& [first, last] : bits) {
        n += last - first;
    }
    return n;
}

RegionBits Random(std::mt19937& rng, unsigned percent) {
    RegionBits bits{};
    std::uniform_int_distribution<unsigned> d(0, 99);
    for (size_t page = 0; page < NUM_PAGES_PER_REGION; ++page) {
        if (d(rng) < percent) {
            bits.Set(page);
        }
    }
    return bits;
}

// One region's CPU tracking: the protection changes and every page watch they lifted.
struct Model {
    RegionBits cpu, gpu, writeable, lifted;
    int changes = 0;
    bool lifted_twice = false;

    Model(const RegionBits& cpu_, const RegionBits& gpu_)
        : cpu{cpu_}, gpu{gpu_}, writeable{cpu_}, lifted{} {}

    // UpdateProtection<false, false>: one UpdatePageWatchersForRegion call for the mask.
    void Update() {
        const RegionBits mask = cpu ^ writeable;
        if (mask.None()) {
            return;
        }
        lifted_twice = lifted_twice || (lifted & mask).Any();
        lifted |= mask;
        writeable = cpu;
        ++changes;
    }
    // ChangeRegionState<CPU, true> over [first, last).
    void MarkCpu(size_t first, size_t last) {
        cpu.SetRange(first, last);
        Update();
    }
    // MemoryTracker::MarkCleanPagesCpuModified: runs of pages, a GPU-modified page skipped only
    // with readbacks on.
    void PreviousWindow(size_t start, size_t end, bool readbacks) {
        size_t run = start;
        bool in_run = false;
        for (size_t page = start; page < end; ++page) {
            if (readbacks && gpu.Get(page)) {
                if (in_run) {
                    MarkCpu(run, page);
                }
                in_run = false;
                continue;
            }
            if (!in_run) {
                run = page;
                in_run = true;
            }
        }
        if (in_run) {
            MarkCpu(run, end);
        }
    }
    // RegionManager::MarkFaultWindow.
    void Window(size_t start, size_t end, bool keep_gpu) {
        const RegionBits add = FaultWindowPages(cpu, gpu, start, end, keep_gpu);
        if (add.None()) {
            return;
        }
        cpu |= add;
        Update();
    }
};

struct Fault {
    RegionBits cpu, gpu;
    size_t start, end, page;
};

Fault RandomFault(std::mt19937& rng) {
    std::uniform_int_distribution<size_t> window(0, NUM_PAGES_PER_REGION / WindowPages - 1);
    std::uniform_int_distribution<size_t> offset(0, WindowPages - 1);
    std::uniform_int_distribution<unsigned> density(0, 100);
    Fault f{Random(rng, density(rng)), Random(rng, density(rng)), 0, 0, 0};
    f.start = window(rng) * WindowPages;
    f.end = f.start + WindowPages;
    f.page = f.start + offset(rng);
    return f;
}

TEST(FaultWindow, WindowFirstEndsAsThePreviousOrderWithReadbacksOff) {
    std::mt19937 rng(1);
    int saved = 0;
    for (int i = 0; i < 20000; ++i) {
        const Fault f = RandomFault(rng);
        Model previous(f.cpu, f.gpu);
        previous.MarkCpu(f.page, f.page + 1); // InvalidateRegion: readbacks off, no flush
        previous.PreviousWindow(f.start, f.end, false);
        Model now(f.cpu, f.gpu);
        now.Window(f.start, f.end, false);
        now.MarkCpu(f.page, f.page + 1);
        EXPECT_TRUE(now.cpu == previous.cpu);
        EXPECT_TRUE(now.writeable == previous.writeable);
        EXPECT_TRUE(now.lifted == previous.lifted);
        EXPECT_FALSE(now.lifted_twice);
        EXPECT_FALSE(previous.lifted_twice);
        EXPECT_TRUE(now.changes <= 1); // the page's own invalidation finds it lifted
        EXPECT_TRUE(now.changes <= previous.changes);
        saved += previous.changes - now.changes;
    }
    EXPECT_GT(saved, 0);
}

TEST(FaultWindow, KeepGpuLeavesOnlyTheGpuPagesProtected) {
    std::mt19937 rng(2);
    for (int i = 0; i < 20000; ++i) {
        const Fault f = RandomFault(rng);
        Model previous(f.cpu, f.gpu);
        previous.MarkCpu(f.page, f.page + 1);
        previous.PreviousWindow(f.start, f.end, false);
        Model now(f.cpu, f.gpu);
        now.Window(f.start, f.end, true); // FAST_CPU_FAULT_WINDOW_KEEP_GPU, readbacks off
        now.MarkCpu(f.page, f.page + 1);
        // The previous order marked them too; KEEP_GPU keeps the window's other GPU pages.
        RegionBits kept(~f.cpu, f.start, f.end);
        kept &= f.gpu;
        kept.Unset(f.page);
        EXPECT_TRUE((previous.cpu ^ now.cpu) == kept);
        EXPECT_TRUE(now.writeable == now.cpu);
        EXPECT_FALSE(now.lifted_twice);
    }
}

TEST(FaultWindow, ReadbacksOnKeepThePreviousOrderAndPages) {
    std::mt19937 rng(3);
    for (int i = 0; i < 20000; ++i) {
        const Fault f = RandomFault(rng);
        if (f.gpu.Get(f.page)) {
            continue; // the page flushes first: the download is out of this model
        }
        Model previous(f.cpu, f.gpu);
        previous.MarkCpu(f.page, f.page + 1);
        previous.PreviousWindow(f.start, f.end, true);
        Model now(f.cpu, f.gpu);
        now.MarkCpu(f.page, f.page + 1); // with readbacks on the page goes first, as before
        now.Window(f.start, f.end, true);
        EXPECT_TRUE(now.cpu == previous.cpu);
        EXPECT_TRUE(now.writeable == previous.writeable);
        EXPECT_TRUE(now.lifted == previous.lifted);
        EXPECT_TRUE(now.changes <= 2); // the page, then every run of the window at once
        EXPECT_TRUE(now.changes <= previous.changes);
    }
}

TEST(FaultWindow, VerifyFindsNoMismatchInAnyMode) {
    std::mt19937 rng(4);
    for (int i = 0; i < 20000; ++i) {
        const Fault f = RandomFault(rng);
        const bool readbacks = i & 1;
        const bool keep_gpu_switch = i & 2;
        const bool keep_gpu = readbacks || keep_gpu_switch;
        const RegionBits add = FaultWindowPages(f.cpu, f.gpu, f.start, f.end, keep_gpu);
        FaultWindowCounts counts{};
        CompareFaultWindow(counts, f.cpu, f.gpu, add, f.start, f.end, readbacks, keep_gpu, f.page);
        EXPECT_EQ(counts.mismatched, 0u);
        EXPECT_EQ(counts.marked, Pages(add));
        EXPECT_EQ(counts.previous, counts.marked + counts.keep_gpu);
        RegionBits guarded_gpu(~f.cpu, f.start, f.end);
        guarded_gpu &= f.gpu;
        EXPECT_EQ(counts.keep_gpu, keep_gpu_switch && !readbacks ? Pages(guarded_gpu) : 0u);
        EXPECT_EQ(counts.gpu_kept, keep_gpu ? Pages(guarded_gpu) : 0u);
        EXPECT_EQ(counts.gpu_marked, keep_gpu ? 0u : Pages(guarded_gpu));
        // The faulting page: lifted unless it is a kept GPU page that was protected.
        const bool kept = keep_gpu && f.gpu.Get(f.page) && !f.cpu.Get(f.page);
        EXPECT_EQ(counts.fault_kept, kept ? 1u : 0u);
        EXPECT_EQ(counts.fault_lifted, kept ? 0u : 1u);
    }
}

TEST(FaultWindow, VerifyFindsAWrongWindow) {
    std::mt19937 rng(5);
    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        const Fault f = RandomFault(rng);
        const bool readbacks = i & 1;
        RegionBits add = FaultWindowPages(f.cpu, f.gpu, f.start, f.end, readbacks);
        const auto mismatched = [&](const RegionBits& window) {
            FaultWindowCounts counts{};
            CompareFaultWindow(counts, f.cpu, f.gpu, window, f.start, f.end, readbacks, readbacks,
                               ~size_t{});
            return counts.mismatched;
        };
        if (add.Any()) {
            RegionBits dropped = add; // a page left protected
            dropped.Unset(add.FirstRange().first);
            EXPECT_EQ(mismatched(dropped), 1u);
            ++checked;
        }
        RegionBits extra = add; // a page outside the window marked too
        const size_t outside = f.end % NUM_PAGES_PER_REGION;
        if (!f.cpu.Get(outside)) {
            extra.Set(outside);
            EXPECT_EQ(mismatched(extra), 1u);
        }
        RegionBits gpu_window(~f.cpu, f.start, f.end); // GPU pages marked with readbacks on
        gpu_window &= f.gpu;
        if (readbacks && gpu_window.Any()) {
            EXPECT_EQ(mismatched(add | gpu_window), Pages(gpu_window));
        }
    }
    EXPECT_GT(checked, 0);
}

TEST(FaultWindow, AFaultOutsideTheWindowIsNotCounted) {
    RegionBits cpu{}, gpu{};
    const RegionBits add = FaultWindowPages(cpu, gpu, 16, 32, false);
    FaultWindowCounts counts{};
    CompareFaultWindow(counts, cpu, gpu, add, 16, 32, false, false, 32);
    CompareFaultWindow(counts, cpu, gpu, add, 16, 32, false, false, ~size_t{});
    EXPECT_EQ(counts.fault_lifted + counts.fault_kept, 0u);
    EXPECT_EQ(counts.marked, 32u); // two windows of 16 pages
}

} // namespace
