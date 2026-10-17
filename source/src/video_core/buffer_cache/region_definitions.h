// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {

constexpr u64 TRACKER_PAGE_BITS = 12; // 4K pages
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22; // each region is 4MB
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

/// FAST_CPU_FAULT_WINDOW: the pages of [start_page, end_page) a write-fault window marks CPU
/// modified, all in one protection change: the write-protected (CPU-clean) ones, less the
/// GPU-modified ones when `keep_gpu` (readbacks on, or FAST_CPU_FAULT_WINDOW_KEEP_GPU).
inline RegionBits FaultWindowPages(const RegionBits& cpu, const RegionBits& gpu, size_t start_page,
                                   size_t end_page, bool keep_gpu) {
    const RegionBits guarded(~cpu, start_page, end_page);
    return keep_gpu ? guarded & ~gpu : guarded;
}

/// FAST_CPU_FAULT_WINDOW_VERIFY: the pages the previous window (MemoryTracker::
/// MarkCleanPagesCpuModified) changed, worked out its own way: page by page, a GPU-modified page
/// skipped only with readbacks on, every other one marked.
inline RegionBits PreviousFaultWindowPages(const RegionBits& cpu, const RegionBits& gpu,
                                           size_t start_page, size_t end_page, bool readbacks) {
    RegionBits marked = cpu;
    for (size_t page = start_page; page < end_page; ++page) {
        if (!(readbacks && gpu.Get(page))) {
            marked.Set(page);
        }
    }
    return marked ^ cpu;
}

/// FAST_CPU_FAULT_WINDOW_VERIFY: what one write-fault window did (RegionManager::MarkFaultWindow)
/// against what the previous window would have done to the same pages.
struct FaultWindowCounts {
    u64 fault{};        // in: the faulting address
    u64 marked{};       // write-protected pages marked CPU modified (unprotected)
    u64 gpu_marked{};   // ... of them GPU-modified (readbacks off, as the previous window)
    u64 gpu_kept{};     // write-protected GPU-modified pages left (readbacks, or KEEP_GPU)
    u64 previous{};     // pages the previous window would have marked
    u64 keep_gpu{};     // ... that KEEP_GPU keeps instead (a difference by design)
    u64 mismatched{};   // pages where the two windows disagree otherwise
    u64 fault_lifted{}; // the faulting page is CPU modified after the window, so its own
                        // invalidation changes no protection
    u64 fault_kept{};   // ... still write-protected (GPU-modified and kept)
};

/// FAST_CPU_FAULT_WINDOW_VERIFY: adds to `counts` the window `add` (FaultWindowPages) against
/// PreviousFaultWindowPages, both for the state before either changed anything. `fault_page`:
/// the faulting page's index in the region (outside the window: not counted).
inline void CompareFaultWindow(FaultWindowCounts& counts, const RegionBits& cpu,
                               const RegionBits& gpu, const RegionBits& add, size_t start_page,
                               size_t end_page, bool readbacks, bool keep_gpu, size_t fault_page) {
    const auto pages = [](const RegionBits& bits) {
        u64 n = 0;
        for (const auto& [first, last] : bits) {
            n += last - first;
        }
        return n;
    };
    const RegionBits guarded(~cpu, start_page, end_page);
    const RegionBits previous = PreviousFaultWindowPages(cpu, gpu, start_page, end_page, readbacks);
    // KEEP_GPU with readbacks off keeps the GPU-modified pages the previous window marked.
    const RegionBits expected = keep_gpu && !readbacks ? previous & ~gpu : previous;
    counts.marked += pages(add);
    counts.gpu_marked += pages(add & gpu);
    counts.gpu_kept += pages(guarded & gpu & ~add);
    counts.previous += pages(previous);
    counts.keep_gpu += pages(previous ^ expected);
    counts.mismatched += pages(add ^ expected);
    if (fault_page >= start_page && fault_page < end_page) {
        ++(cpu.Get(fault_page) || add.Get(fault_page) ? counts.fault_lifted : counts.fault_kept);
    }
}

} // namespace VideoCore
