// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

    /// FAST_CPU_FAULT_WINDOW (default on, '0' off; with FAST bit 0, native stereo only): a guest
    /// write fault lifts its window's buffer protection in one change (the same pages as
    /// before), with readbacks off before its own page (which then needs no change of its own),
    /// and checks the mapping once (Vulkan::Rasterizer::InvalidateWriteFault). Read once.
    static bool ExactFaultWindow();
    /// FAST_CPU_FAULT_WINDOW_VERIFY=1: also works out each window's pages the previous way and
    /// compares, counts the protection changes the faulting page's own invalidation still made,
    /// and which threads take the faults. Read once.
    static bool ExactFaultWindowVerify();
    /// FAST_CPU_FAULT_WINDOW_KEEP_GPU (default on, '0' off; with FastRender::FaultWindow): with
    /// readbacks off a window leaves GPU-modified pages protected (bbport's mask), so a write
    /// fault elsewhere in the window does not replace their GPU data with the guest's bytes at
    /// the next upload; a CPU write to such a page itself still faults on its own, as without
    /// the window. Off, the window lifted them too: data only the GPU ever wrote (the face's
    /// FaceGen-morphed eyelash and brow positions, written once per load) was replaced by
    /// whatever the freed memory held before (an old map piece's vertices: triangles to
    /// infinity from the hunter's head after a fast travel, 2026-10-15). Read once.
    static bool FaultWindowKeepGpu();
    /// FAST_CPU_FAULT_WINDOW_VERIFY: the buffer protection changes this thread has made
    /// (UpdatePageWatchersForRegion for writes).
    static u64 WatcherUpdates() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
