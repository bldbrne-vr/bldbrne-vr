#include "video_core/host_copies.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>

#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"

#ifdef __unix__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

/// FastRender 17: lock-free IsRegionGpuModified (bbport memory_tracker.h:65-70).
/// FastRender 24: upload early exits (bbport LockFreeUploadCheck / PageTrackingEarlyExit).
inline constexpr auto LockFreeDirtyQuery = static_cast<FastRender::Bit>(17);
inline constexpr auto UploadEarlyExit = static_cast<FastRender::Bit>(24);

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SpinLock;
#endif

/**
 * Allows tracking CPU and GPU modification of pages in a contigious 16MB virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
        for (auto& epoch : epochs) {
            epoch.store(1, std::memory_order_relaxed);
        }
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    /// perf_copy_memo (vk_shader_hle.cpp): each 64-page (256 KB) word of the region has an epoch,
    /// bumped under the lock whenever a page in it changes its CPU or GPU state (a CPU write
    /// noticed, an upload that clears it, a GPU write marked for the first time, a GPU mark
    /// cleared). The epochs only grow, so the sum over a range is unchanged exactly while no page
    /// in the range's words changed state. offset / size: bytes within the region.
    static constexpr size_t EPOCH_WORDS = NUM_PAGES_PER_REGION / 64;
    [[nodiscard]] u64 EpochSum(u64 offset, u64 size) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        u64 sum = 0;
        for (size_t w = start_page / 64; start_page < end_page && w <= (end_page - 1) / 64; ++w) {
            sum += epochs[w].load(std::memory_order_acquire);
        }
        return sum;
    }

    /// True when every page of the range is marked GPU-modified (perf_copy_memo's check).
    [[nodiscard]] bool IsRegionAllGpuModified(u64 offset, u64 size) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        if (start_page >= end_page) {
            return true;
        }
        const RegionBits missing(~gpu, start_page, end_page);
        return missing.None();
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        // Region lock spans upload publication. Retire readers before allowing a
        // guest write or making GPU-written pages unreadable to copy workers.
        if constexpr (enable)
            FastRender::WaitHostCopies(FastRender::CopyWait::Protection, dirty_addr, size);
        RegionBits& bits = GetRegionBits<type>();
        if constexpr (type == Type::GPU && enable) {
            // A GPU write marked again on pages already marked changes nothing (perf_copy_memo).
            if (RegionBits(~bits, start_page, end_page).Any()) {
                BumpEpochs(start_page, end_page);
            }
        } else {
            BumpEpochs(start_page, end_page);
        }
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            UpdateProtection<!enable, false>();
        } else if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
            UpdateProtection<enable, true>();
        }
    }

    /// FAST_CPU_FAULT_WINDOW (bbport ExtendWriteFault): after a guest write fault, marks as CPU
    /// modified the write-protected pages of the window in one pass and one protection update,
    /// as a write fault on each would. Which pages, as MemoryTracker::MarkCleanPagesCpuModified:
    /// GPU-modified pages keep their state with readbacks on (a write there still faults and
    /// flushes first) or FAST_CPU_FAULT_WINDOW_KEEP_GPU (the default: a write there still faults
    /// on its own); with both off they are marked too, and the next upload replaces their GPU
    /// data with the guest's bytes.
    /// offset / size: bytes within the region. `counts` (verify only) gets the page counts, and
    /// the comparison with the previous window's pages, worked out its own way.
    void MarkFaultWindow(u64 offset, u64 size, FaultWindowCounts* counts) {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        if (start_page >= end_page) {
            return;
        }
        const bool readbacks = EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled;
        const bool keep_gpu = readbacks || PageManager::FaultWindowKeepGpu();
        // The CPU-clean pages: write-protected by this tracker (`writeable` mirrors `cpu`).
        const RegionBits add = FaultWindowPages(cpu, gpu, start_page, end_page, keep_gpu);
        if (counts) [[unlikely]] {
            // The faulting page's index here (an address below the region wraps out of range).
            const u64 fault = counts->fault - cpu_addr;
            CompareFaultWindow(*counts, cpu, gpu, add, start_page, end_page, readbacks, keep_gpu,
                               fault < TRACKER_HIGHER_PAGE_SIZE ? fault / TRACKER_BYTES_PER_PAGE
                                                                : ~size_t{});
        }
        if (add.None()) {
            return;
        }
        // As ChangeRegionState: copy workers' reads of these pages retire before guest writes.
        FastRender::WaitHostCopies(FastRender::CopyWait::Protection,
                                   cpu_addr + start_page * TRACKER_BYTES_PER_PAGE,
                                   (end_page - start_page) * TRACKER_BYTES_PER_PAGE);
        BumpEpochs(start_page, end_page); // perf_copy_memo: these pages changed their CPU state
        cpu |= add;
        UpdateProtection<false, false>();
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        // Bit 24, lock held: no CPU bit in range means UnsetRange is a no-op and
        // UpdateProtection finds cpu == writeable, so skip the mask copy and the stores.
        if constexpr (type == Type::CPU)
            if (FastRender::On(UploadEarlyExit) &&
                !bits.AnyRange(start_page, std::min<size_t>(end_page, NUM_PAGES_PER_REGION)))
                return;
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            if (mask.Any()) {
                BumpEpochs(start_page, end_page); // perf_copy_memo
            }
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                UpdateProtection<true, false>();
            } else if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    /// Bits 17/24: IsRegionModified<type> (all=false) / IsRegionAllGpuModified (all=true), unlocked.
    template <Type type, bool all = false>
    [[nodiscard]] bool TestRelaxed(u64 offset, u64 size) const noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        return GetRegionBits<type>().template TestRangeRelaxed<all>(start_page, end_page);
    }

    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        static const bool direct = std::getenv("VR_DIRECT_DIRTY_RANGE") != nullptr;
        if (direct) {
            const bool result = bits.AnyRange(start_page, end_page);
            static const bool check = std::getenv("VR_PERF_CHECK") != nullptr;
            if (check) {
                ASSERT_MSG(result == RegionBits(bits, start_page, end_page).Any(),
                           "Direct dirty range mismatch");
            }
            return result;
        }
        RegionBits test(bits, start_page, end_page);
        return test.Any();
    }

    LockType lock;

private:
    void BumpEpochs(size_t start_page, size_t end_page) noexcept {
        for (size_t w = start_page / 64; w <= (end_page - 1) / 64 && w < EPOCH_WORDS; ++w) {
            epochs[w].fetch_add(1, std::memory_order_acq_rel);
        }
    }

    /**
     * Notify tracker about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the tracker
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam track True when the tracker should start tracking the new pages
     */
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        RegionBits mask = is_read ? (~gpu ^ readable) : (cpu ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = cpu;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
    std::array<std::atomic<u64>, EPOCH_WORDS> epochs{}; // perf_copy_memo (EpochSum)
};

} // namespace VideoCore
