// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/memory_write_trace.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/kept_page_upload.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

#ifdef BBVR_NO_LOGS
using KeptLock = std::mutex;
#else
/// Freeze hunt (2026-10-18, logs-on builds only): kept_lock remembers its holder and its last
/// waiter, and MemoryTracker's watchdog logs a hold past 3 s with their thread ids (the freeze
/// when the intro movie is skipped: StageB waiting in its upload, every thread idle). Match the
/// ids to a stack dump of the frozen game.
struct KeptLock {
    static s64 Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void lock() {
        if (!m.try_lock()) {
            waiter.store(Common::CurrentThreadId(), std::memory_order_relaxed);
            waiters.fetch_add(1, std::memory_order_relaxed);
            m.lock();
            waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        since.store(Now(), std::memory_order_relaxed);
        owner.store(Common::CurrentThreadId(), std::memory_order_relaxed);
    }
    bool try_lock() {
        if (!m.try_lock()) {
            return false;
        }
        since.store(Now(), std::memory_order_relaxed);
        owner.store(Common::CurrentThreadId(), std::memory_order_relaxed);
        return true;
    }
    void unlock() {
        owner.store(0, std::memory_order_relaxed);
        m.unlock();
    }
    std::mutex m;
    std::atomic<u32> owner{0};  // the holder's thread id (0: free)
    std::atomic<s64> since{0};  // when it was taken
    std::atomic<u32> waiter{0}; // the last thread that had to wait
    std::atomic<u32> waiters{0};
};
#endif

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {
#ifndef BBVR_NO_LOGS
        hang_watch = std::jthread([this](std::stop_token stop) { WatchKeptLock(stop); });
#endif
    }
    ~MemoryTracker() = default;

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        if (FastRender::On(LockFreeDirtyQuery))
            return IteratePages<false>(query_cpu_addr, query_size,
                                       [](RegionManager* m, u64 o, size_t s) {
                                           return Relaxed<Type::GPU>(m, o, s);
                                       });
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// FAST bits 33/34 (ObtainSmallDirect, BindIndexBuffer): true when every page of the range is tracked
    /// and CPU-clean, by bit 17's unlocked test. The buffer registered over a CPU-clean page holds
    /// its bytes (its upload cleared the page), so ObtainBuffer would upload nothing there.
    bool IsRegionCpuCleanRelaxed(VAddr query_cpu_addr, u64 query_size) noexcept {
        u64 remaining = query_size;
        size_t page_index = query_cpu_addr >> TRACKER_HIGHER_PAGE_BITS;
        u64 page_offset = query_cpu_addr & TRACKER_HIGHER_PAGE_MASK;
        while (remaining > 0) {
            const u64 amount = std::min<u64>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining);
            RegionManager* const manager =
                page_index < NUM_HIGH_PAGES ? top_tier[page_index].load(std::memory_order_acquire)
                                            : nullptr;
            // An untracked region counts as CPU-modified (a new region starts with every page so).
            if (manager == nullptr || Relaxed<Type::CPU>(manager, page_offset, amount)) {
                return false;
            }
            ++page_index;
            page_offset = 0;
            remaining -= amount;
        }
        return query_size != 0;
    }

    /// perf_copy_memo (vk_shader_hle.cpp): the sum of the write-tracking epochs of the 256 KB
    /// words the range touches (RegionManager::EpochSum); 0 when a region of it is not tracked
    /// yet. Unchanged exactly while no page there changed its CPU or GPU state. Lock-free.
    u64 RangeEpoch(VAddr query_cpu_addr, u64 query_size) const noexcept {
        u64 sum = 0;
        u64 remaining = query_size;
        size_t page_index = query_cpu_addr >> TRACKER_HIGHER_PAGE_BITS;
        u64 page_offset = query_cpu_addr & TRACKER_HIGHER_PAGE_MASK;
        while (remaining > 0) {
            const u64 amount = std::min<u64>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining);
            if (page_index >= NUM_HIGH_PAGES || top_tier[page_index] == nullptr) {
                return 0;
            }
            sum += top_tier[page_index].load(std::memory_order_acquire)->EpochSum(page_offset, amount);
            ++page_index;
            page_offset = 0;
            remaining -= amount;
        }
        return sum; // every word's epoch starts at 1: a tracked, non-empty range is never 0
    }

    /// True when every page of the range is marked GPU-modified (perf_copy_memo's check).
    bool IsRegionAllGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        bool all = true;
        IteratePages<false>(query_cpu_addr, query_size,
                            [&all](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                all = all && manager->IsRegionAllGpuModified(offset, size);
                            });
        // A region not tracked at all has no GPU-modified page.
        u64 remaining = query_size;
        size_t page_index = query_cpu_addr >> TRACKER_HIGHER_PAGE_BITS;
        u64 page_offset = query_cpu_addr & TRACKER_HIGHER_PAGE_MASK;
        while (all && remaining > 0) {
            const u64 amount = std::min<u64>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining);
            all = page_index < NUM_HIGH_PAGES && top_tier[page_index] != nullptr;
            ++page_index;
            page_offset = 0;
            remaining -= amount;
        }
        return all;
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Fast renderer (FastRender::FaultWindow): marks the pages of the range as CPU modified,
    /// which lifts their write protection the way a write fault on each would, without the
    /// faults. GPU-modified pages keep their state with readbacks on (a write there still faults
    /// and flushes first) or FAST_CPU_FAULT_WINDOW_KEEP_GPU (the default: a write there still
    /// faults on its own); otherwise they are marked too, and the next upload replaces their GPU
    /// data with the guest's bytes (see RegionManager::MarkFaultWindow). Untracked pages are
    /// already CPU modified.
    void MarkCleanPagesCpuModified(VAddr cpu_addr, u64 size) {
        IteratePages<false>(cpu_addr, size, [](RegionManager* manager, u64 offset, size_t size) {
            std::scoped_lock lk{manager->lock};
            const bool keep_gpu =
                EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled ||
                PageManager::FaultWindowKeepGpu();
            u64 run_start = offset;
            u64 run_size = 0;
            const auto mark_run = [&] {
                if (run_size != 0) {
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + run_start, run_size);
                    run_size = 0;
                }
            };
            for (u64 page = offset; page < offset + size; page += TRACKER_BYTES_PER_PAGE) {
                if (keep_gpu &&
                    manager->template IsRegionModified<Type::GPU>(page, TRACKER_BYTES_PER_PAGE)) {
                    mark_run();
                    continue;
                }
                if (run_size == 0) {
                    run_start = page;
                }
                run_size += TRACKER_BYTES_PER_PAGE;
            }
            mark_run();
        });
    }

    /// FAST_CPU_FAULT_WINDOW: RegionManager::MarkFaultWindow over each tracked region of the
    /// range (the pages of an untracked region are CPU modified already).
    void MarkFaultWindow(VAddr cpu_addr, u64 size, FaultWindowCounts* counts) {
        IteratePages<false>(cpu_addr, size,
                            [counts](RegionManager* manager, u64 offset, size_t bytes) {
                                std::scoped_lock lk{manager->lock};
                                manager->MarkFaultWindow(offset, bytes, counts);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested.
    /// `write_fault`: a guest write fault at `cpu_addr`, before the write lands
    /// (FAST_GPU_PAGE_KEEP: its GPU-modified pages are kept aside, KeepGpuPages).
    /// `emulator_write`: the emulator's own write of exactly [cpu_addr, cpu_addr + written)
    /// (a file read, a video frame; a protection change writes nothing: `written` 0): the
    /// pages it writes in part are kept aside the same way, its own bytes never.
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush, bool write_fault = false,
                          bool emulator_write = false, u64 written = 0) noexcept {
        IteratePages<false>(
            cpu_addr, size,
            [this, cpu_addr, write_fault, emulator_write, written,
             &on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    if (write_fault && KeepGpuPages()) {
                        KeepGpuPagesAside(manager, offset, size, cpu_addr, cpu_addr + 8, false);
                    } else if (emulator_write && KeepGpuPages()) {
                        KeepGpuPagesAside(manager, offset, size, cpu_addr, cpu_addr + written,
                                          true);
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload, bool async_upload = false) {
        const bool hold = is_written || async_upload;
        // Bit 24 (bbport LockFreeUploadCheck): copy workers rely on the held locks, keep them.
        const bool early = FastRender::On(UploadEarlyExit) && !FastRender::On(FastRender::CopyThreads);
        // Written, CPU-clean, every page already GPU-modified: the locked pass uploads nothing
        // and ChangeRegionState<GPU,true> changes nothing (no new bits, no epoch bump).
        if (early && is_written && !async_upload &&
            !IteratePages<true>(query_cpu_range, query_size, [](RegionManager* m, u64 o, size_t s) {
                return Relaxed<Type::CPU>(m, o, s) || !Relaxed<Type::GPU, true>(m, o, s);
            })) {
            on_upload(false);
            return;
        }
        bool safe_async = async_upload;
        IteratePages<true>(query_cpu_range, query_size,
                          [&func, hold, early, &safe_async](RegionManager* manager, u64 offset, size_t size) {
                              // Read-only and CPU-clean: nothing to upload, no lock needed.
                              if (early && !hold && !Relaxed<Type::CPU>(manager, offset, size))
                                  return;
                              manager->lock.lock();
                              bool copied = false;
                              manager->template ForEachModifiedRange<Type::CPU, true>(
                                  manager->GetCpuAddr() + offset, size, [&](auto address, auto bytes) {
                                      copied = true;
                                      func(address, bytes);
                                  });
                              // Reuse the held region lock, and inspect GPU state only
                              // where CPU-dirty bytes actually need uploading.
                              if (copied && safe_async &&
                                  manager->template IsRegionModified<Type::GPU>(offset, size))
                                  safe_async = false;
                              if (!hold) manager->lock.unlock();
                          });
        if (hold && !is_written && !safe_async) {
            // Synchronous GPU-dirty reads may fault; release region locks first.
            IteratePages<false>(query_cpu_range, query_size,
                [](RegionManager* manager, u64, size_t) { manager->lock.unlock(); });
            on_upload(false);
            return;
        }
        on_upload(safe_async);
        if (!hold) return;
        IteratePages<false>(query_cpu_range, query_size,
                           [is_written](RegionManager* manager, u64 offset, size_t size) {
                               if (is_written) manager->template ChangeRegionState<Type::GPU, true>(
                                   manager->GetCpuAddr() + offset, size);
                               manager->lock.unlock();
                           });
    }

    /// FAST_GPU_PAGE_KEEP (default on, '0' off; readbacks off): with readbacks off, a guest write
    /// fault on a page marks the whole 4 KB page CPU modified, and its next upload replaces every
    /// byte of it with the guest's, also the bytes only the GPU ever wrote (the guest still holds
    /// whatever was there before). Data the GPU writes once into memory that shares a page with
    /// data the CPU writes was lost that way: the face's FaceGen-morphed positions (written by the
    /// GPU at load, sharing pages with a texture and older data) became an old map piece's
    /// vertices or zeros, grey triangles to infinity from the hunter's head after a load
    /// (2026-10-15). So the fault keeps such a page's bytes aside before the write lands
    /// (InvalidateRegion), and its upload (ForUpload) leaves out each GPU-written byte range of
    /// the page the CPU did not change since (not one byte of it differs); the rest of the page
    /// uploads as before. A range the CPU changed at all uploads whole (memory reused for new
    /// data: no stale GPU byte survives where the new data happens to equal the old guest
    /// bytes). A fault inside the GPU-written bytes (the CPU rewriting them, e.g. resetting a
    /// counter) uploads the whole page as before, and so does a page evicted from the full pool
    /// (KeptPages, the oldest first). The emulator's own writes into guest memory take part the same way (the triangles
    /// came back after a rest-warp at the Central Yharnam lamp, 2026-10-15, with no guest fault
    /// kept since the start: a map load's file reads are the likely door): the bytes it wrote always upload, and a page they cover
    /// whole or a GPU-written range they touch upload whole. An unmap, and a buffer the garbage
    /// collector drops, forget the kept pages of their range (ForgetKept): their next upload is
    /// whole again.
    static bool KeepGpuPages() {
        static const bool on = [] {
            const char* value = std::getenv("FAST_GPU_PAGE_KEEP");
            return !(value && value[0] == '0');
        }();
        return on;
    }

    /// FAST_GPU_PAGE_KEEP: the kept pages of [address, address + bytes) upload whole again.
    void ForgetKept(VAddr address, u64 bytes) {
        if (kept_count.load(std::memory_order_acquire) == 0) [[likely]] {
            return;
        }
        std::scoped_lock lk{kept_lock};
        const VAddr first = Common::AlignDown(address, TRACKER_BYTES_PER_PAGE);
        const VAddr end = address + bytes;
        if ((end - first) / TRACKER_BYTES_PER_PAGE <= 64) {
            for (VAddr page = first; page < end; page += TRACKER_BYTES_PER_PAGE) {
                if (KeptPage* kept = FindKept(page); kept != nullptr) {
                    ReleaseKept(*kept);
                }
            }
            return;
        }
        // Over the compact slot -> page index (64 KB), not the 33 MB of kept pages.
        for (u32 slot = 0; slot < KeptPages; ++slot) {
            const VAddr page = (*kept_slot_page)[slot];
            if (page != 0 && page >= first && page < end) {
                ReleaseKept((*kept_pages)[slot]);
            }
        }
    }

    /// ForEachUploadRange's `func` for [address, address + bytes): calls `emit(address, bytes)`
    /// for what to upload. `gpu_ranges(page, size, f)` calls f(start, end) for each GPU-written
    /// byte range of the page (BufferCache::gpu_modified_ranges). The pages are write-protected
    /// again by then (ForEachModifiedRange), so later guest writes fault anew.
    /// `lost(start, end, why)`: GPU-written bytes this upload replaces with the guest's (the
    /// catch-all log of how the ghost triangles' GPU-written data could still be lost: a page
    /// that was not kept aside, a range the CPU wrote in, a range it changed).
    template <typename Emit, typename GpuRanges, typename Lost>
    void ForUpload(VAddr address, u64 bytes, Emit&& emit, GpuRanges&& gpu_ranges, Lost&& lost) {
        const auto plain_emit = [&](VAddr at, u64 size) {
            emit(at, size);
#ifdef BBVR_MEMORY_WRITE_TRACE
            gpu_ranges(at, size, [&](VAddr start, VAddr stop) {
                lost(std::max(start, at), std::min(stop, at + size), LostWhy::NotKept);
            });
#endif
        };
        if (kept_count.load(std::memory_order_acquire) == 0) [[likely]] {
            plain_emit(address, bytes);
            return;
        }
        std::scoped_lock lk{kept_lock};
        const VAddr end = address + bytes;
        VAddr plain = address; // the start of the run not yet emitted that has no kept page
        for (VAddr page = Common::AlignDown(address, TRACKER_BYTES_PER_PAGE); page < end;
             page += TRACKER_BYTES_PER_PAGE) {
            KeptPage* kept = FindKept(page);
            if (kept == nullptr) {
                continue;
            }
            const VAddr lo = std::max(page, address);
            const VAddr hi = std::min(page + TRACKER_BYTES_PER_PAGE, end);
            if (plain < lo) {
                plain_emit(plain, lo - plain);
            }
            plain = hi;
            EmitKeptPage(*kept, lo, hi, emit, gpu_ranges, lost);
            ReleaseKept(*kept);
        }
        if (plain < end) {
            plain_emit(plain, end - plain);
        }
    }

    /// Why ForUpload replaced GPU-written bytes (its `lost` callback).
    enum class LostWhy : u8 {
        NotKept,     // the page was not kept aside (no keep for the path that made it CPU modified)
        WroteInside, // the CPU or the emulator wrote inside the GPU-written range
        Changed,     // the CPU changed bytes of the range since the page was kept aside
    };
    static constexpr const char* LostName(LostWhy why) {
        switch (why) {
        case LostWhy::NotKept:
            return "page not kept aside";
        case LostWhy::WroteInside:
            return "written inside the range";
        case LostWhy::Changed:
            return "changed by the CPU";
        }
        return "?";
    }
    /// Pages evicted from the full pool (the oldest: they upload whole).
    u64 KeptPoolFull() const {
        return kept_evictions.load(std::memory_order_relaxed);
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    // FAST_GPU_PAGE_KEEP: the pages kept aside, in a pool allocated once (the fault handler does
    // not allocate), found through an open-addressed hash (kept_hash, page -> slot). A page
    // leaves the pool at its upload; one never uploaded again (memory the game rewrote and no
    // longer draws) would stay: when the pool is full the oldest page is evicted (it uploads
    // whole, as with no keep). 2026-10-15: 256 pages and no eviction: from a lamp's travel on the
    // pool stayed full of pages never drawn again (32769 pages refused in 7 minutes), and the
    // hunter's face lost its GPU data again.
    // 8192 pages (33 MB): about 75 pages a second are kept aside for memory never drawn again
    // (measured 2026-10-15), so a page kept aside lives about 100 s before it can be evicted,
    // longer than any loading screen during which the face is not drawn.
    static constexpr size_t KeptPages = 8192;
    static constexpr size_t KeptBucketBits = 15;
    static constexpr size_t KeptBuckets = size_t{1} << KeptBucketBits; // 4x the pages
    struct KeptPage {
        VAddr page{};
        VAddr write_lo{}; // the bytes written since the page was kept aside (a fault: its 8)
        VAddr write_hi{};
        u64 seq{};        // when it was kept aside (the oldest is evicted)
        bool emulator{};  // ... by the emulator (a file read), not a guest write fault
        std::array<u8, TRACKER_BYTES_PER_PAGE> bytes{};
    };
    struct KeptBucket {
        VAddr page{}; // 0: empty
        u32 slot{};
    };
    struct KeptOrder {
        u32 slot{};
        u64 seq{}; // the slot's seq when it was kept (a different one: released since)
    };
    static constexpr u32 KeptOrderSize = 2 * KeptPages;

    static size_t KeptHome(VAddr page) {
        return static_cast<size_t>(((page >> TRACKER_PAGE_BITS) * 0x9E3779B97F4A7C15ull) >>
                                   (64 - KeptBucketBits));
    }

    KeptPage* FindKept(VAddr page) {
        for (size_t i = KeptHome(page);; i = (i + 1) & (KeptBuckets - 1)) {
            const KeptBucket& b = (*kept_hash)[i];
            if (b.page == 0) {
                return nullptr;
            }
            if (b.page == page) {
                return &(*kept_pages)[b.slot];
            }
        }
    }

    // A slot for `page` (not in the pool): a free one, else the oldest page's (evicted). kept_lock
    // held.
    KeptPage& AcquireKept(VAddr page) {
        u32 slot;
        if (kept_free_count != 0) {
            slot = kept_free[--kept_free_count];
        } else {
            // The oldest page still kept: the ring's front, past slots released since (their
            // seq no longer matches).
            for (;;) {
                const KeptOrder front = (*kept_order)[kept_order_head];
                kept_order_head = (kept_order_head + 1) % KeptOrderSize;
                --kept_order_count;
                if ((*kept_pages)[front.slot].seq == front.seq) {
                    slot = front.slot;
                    break;
                }
            }
            KeptPage& old = (*kept_pages)[slot];
            keep_stats.evicted.fetch_add(1, std::memory_order_relaxed);
            const u64 n = kept_evictions.fetch_add(1, std::memory_order_relaxed);
            if (n < 16 || (n & 1023) == 0) {
                LOG_INFO(Render_Vulkan,
                         "FAST_GPU_PAGE_KEEP: the pool is full ({} pages): the oldest, page "
                         "{:#x}, makes room for {:#x} ({} evictions so far)",
                         KeptPages, old.page, page, n + 1);
            }
            Common::MemoryWriteTrace::Event("gpu.keep.evicted", old.page,
                                           TRACKER_BYTES_PER_PAGE);
            ReleaseKept(old);
            slot = kept_free[--kept_free_count];
        }
        KeptPage& kept = (*kept_pages)[slot];
        kept.page = page;
        kept.seq = ++kept_seq;
        (*kept_slot_page)[slot] = page;
        if (kept_order_count == KeptOrderSize) {
            // Full of released slots' entries: keep the live ones, in order, in place (each is
            // written at or behind where it is read; amortized O(1)).
            u32 kept_live = 0;
            for (u32 i = 0; i < kept_order_count; ++i) {
                const KeptOrder e = (*kept_order)[(kept_order_head + i) % KeptOrderSize];
                if ((*kept_pages)[e.slot].seq == e.seq) {
                    (*kept_order)[(kept_order_head + kept_live++) % KeptOrderSize] = e;
                }
            }
            kept_order_count = kept_live;
        }
        (*kept_order)[(kept_order_head + kept_order_count) % KeptOrderSize] = {slot, kept.seq};
        ++kept_order_count;
        size_t i = KeptHome(page);
        while ((*kept_hash)[i].page != 0) {
            i = (i + 1) & (KeptBuckets - 1);
        }
        (*kept_hash)[i] = {page, slot};
        kept_count.fetch_add(1, std::memory_order_release);
        return kept;
    }

    // The page leaves the pool (its upload, ForgetKept, an eviction). kept_lock held.
    void ReleaseKept(KeptPage& kept) {
        if (kept.page == 0) {
            return;
        }
        // The hash: backward-shift deletion (no tombstones).
        size_t i = KeptHome(kept.page);
        while ((*kept_hash)[i].page != kept.page) {
            i = (i + 1) & (KeptBuckets - 1);
        }
        for (size_t j = i;;) {
            j = (j + 1) & (KeptBuckets - 1);
            const VAddr moved = (*kept_hash)[j].page;
            if (moved == 0) {
                break;
            }
            const size_t home = KeptHome(moved);
            // Leave it where it is when its home lies cyclically in (i, j].
            if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) {
                continue;
            }
            (*kept_hash)[i] = (*kept_hash)[j];
            i = j;
        }
        (*kept_hash)[i] = {};
        const u32 slot = static_cast<u32>(&kept - kept_pages->data());
        (*kept_slot_page)[slot] = 0;
        kept.page = 0;
        kept.seq = 0;
        kept_free[kept_free_count++] = slot;
        kept_count.fetch_sub(1, std::memory_order_release);
    }

    static std::array<u32, KeptPages> AllKeptSlots() {
        std::array<u32, KeptPages> slots{};
        for (u32 i = 0; i < KeptPages; ++i) {
            slots[i] = KeptPages - 1 - i;
        }
        return slots;
    }

    // The manager's lock held, before the page's CPU state changes: each GPU-modified page of
    // [offset, offset + size) that is still CPU-clean (write-protected: this is its first write
    // since its last upload) is kept aside as it is now, [lo, hi) the bytes written. An emulator
    // write leaves out the pages it covers whole (none of theirs to keep), and widens the written
    // bytes of a page kept earlier (CPU modified by then: its upload is still to come).
    void KeepGpuPagesAside(RegionManager* manager, u64 offset, size_t size, VAddr lo, VAddr hi,
                           bool emulator) {
        const auto t0 = std::chrono::steady_clock::now();
        KeepGpuPagesAsideImpl(manager, offset, size, lo, hi, emulator);
        const s64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        keep_stats.calls.fetch_add(1, std::memory_order_relaxed);
        keep_stats.ns.fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
        keep_stats.max_ns.store(std::max<u64>(keep_stats.max_ns.load(std::memory_order_relaxed),
                                              static_cast<u64>(ns)),
                                std::memory_order_relaxed);
        MaybeLogKeepStats();
    }

    // Bound copying while holding the region lock. Exceeding this limit still needs
    // a correctness-preserving fallback; do not replace it with unbounded fault work.
    static constexpr u32 KeepCapPerCall = 32;

    // Once a second while pages are kept aside: what the keep cost (the sewers' stutter hunt,
    // 2026-10-15; logs-on builds only print it).
    struct KeepStats {
        std::atomic<u64> kept{0}, evicted{0}, capped{0}, calls{0}, ns{0}, max_ns{0};
    };
    KeepStats keep_stats;
    std::atomic<s64> keep_log_ns{0};
    void MaybeLogKeepStats() {
        const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = keep_log_ns.load(std::memory_order_relaxed);
        if (last == 0) {
            keep_log_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
            return;
        }
        if (now - last < 1'000'000'000 ||
            !keep_log_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return;
        }
        const u64 kept = keep_stats.kept.exchange(0, std::memory_order_relaxed);
        const u64 evicted = keep_stats.evicted.exchange(0, std::memory_order_relaxed);
        const u64 capped = keep_stats.capped.exchange(0, std::memory_order_relaxed);
        const u64 calls = keep_stats.calls.exchange(0, std::memory_order_relaxed);
        const u64 ns = keep_stats.ns.exchange(0, std::memory_order_relaxed);
        const u64 max_ns = keep_stats.max_ns.exchange(0, std::memory_order_relaxed);
        if (kept == 0 && evicted == 0 && capped == 0) {
            return;
        }
        LOG_INFO(Render_Vulkan,
                 "FAST_GPU_PAGE_KEEP: last {:.1f} s: {} pages kept aside, {} evicted, {} over the "
                 "cap; {} writes took {:.2f} ms (max {:.3f} ms); {} kept now",
                 static_cast<double>(now - last) * 1e-9, kept, evicted, capped, calls,
                 static_cast<double>(ns) * 1e-6, static_cast<double>(max_ns) * 1e-6,
                 kept_count.load(std::memory_order_relaxed));
    }

    void KeepGpuPagesAsideImpl(RegionManager* manager, u64 offset, size_t size, VAddr lo, VAddr hi,
                               bool emulator) {
        u32 kept_now = 0;
        const u64 first = Common::AlignDown(offset, TRACKER_BYTES_PER_PAGE);
        // The pages an emulator write covers whole: one pass over the pool for all of them (a map
        // load's file reads span megabytes).
        // (Within this region's part of the range: the callback runs once per region.)
        const VAddr region_lo = manager->GetCpuAddr() + first;
        const VAddr region_hi = manager->GetCpuAddr() + offset + size;
        const VAddr whole_lo = std::max(Common::AlignUp(lo, TRACKER_BYTES_PER_PAGE), region_lo);
        const VAddr whole_hi =
            std::min(Common::AlignDown(hi, TRACKER_BYTES_PER_PAGE),
                     Common::AlignUp(region_hi, TRACKER_BYTES_PER_PAGE));
        if (emulator && whole_hi > whole_lo) {
            ForgetKept(whole_lo, whole_hi - whole_lo);
        }
        for (u64 at = first; at < offset + size; at += TRACKER_BYTES_PER_PAGE) {
            const VAddr page = manager->GetCpuAddr() + at;
            if (emulator && page >= whole_lo && page < whole_hi) {
                continue;
            }
            if (!manager->template IsRegionModified<Type::GPU>(at, TRACKER_BYTES_PER_PAGE)) {
                continue;
            }
            if (manager->template IsRegionModified<Type::CPU>(at, TRACKER_BYTES_PER_PAGE)) {
                if (emulator && hi > lo && kept_count.load(std::memory_order_acquire) != 0) {
                    std::scoped_lock lk{kept_lock};
                    if (KeptPage* kept = FindKept(page); kept != nullptr) {
                        kept->write_lo = kept->write_hi > kept->write_lo
                                             ? std::min(kept->write_lo, lo)
                                             : lo;
                        kept->write_hi = std::max(kept->write_hi, hi);
                        kept->emulator = true;
                    }
                }
                continue;
            }
            if (kept_now >= KeepCapPerCall) {
                keep_stats.capped.fetch_add(1, std::memory_order_relaxed);
                Common::MemoryWriteTrace::Event("gpu.keep.capped", page,
                                               TRACKER_BYTES_PER_PAGE);
                continue;
            }
            ++kept_now;
            keep_stats.kept.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk{kept_lock};
            KeptPage* kept = FindKept(page);
            if (kept == nullptr) {
                kept = &AcquireKept(page);
            }
            kept->write_lo = lo;
            kept->write_hi = hi;
            kept->emulator = emulator;
            std::memcpy(kept->bytes.data(), reinterpret_cast<const void*>(page),
                        TRACKER_BYTES_PER_PAGE);
        }
    }

    // ForUpload, kept_lock held: [lo, hi) of a kept page.
    // A range the write touched uploads whole (2026-10-15: until then a write inside any of
    // them uploaded the whole page, every other GPU range of it too).
    template <typename Emit, typename GpuRanges, typename Lost>
    void EmitKeptPage(const KeptPage& kept, VAddr lo, VAddr hi, Emit& emit, GpuRanges& gpu_ranges,
                      Lost& lost) {
        const u64 kept_bytes = EmitKeptPageUpload(
            kept.page, TRACKER_BYTES_PER_PAGE, kept.bytes.data(),
            reinterpret_cast<const void*>(kept.page), kept.write_lo, kept.write_hi,
            lo, hi, gpu_ranges, emit, [&](VAddr start, VAddr stop, bool touched) {
                lost(start, stop, touched ? LostWhy::WroteInside : LostWhy::Changed);
            });
        if (kept_bytes != 0) {
            Common::MemoryWriteTrace::Event("gpu.keep.preserved", kept.page, kept_bytes);
            const u64 n = kept_logs.fetch_add(1, std::memory_order_relaxed);
            if (n < 64 || (n & 1023) == 0) {
                LOG_INFO(Render_Vulkan,
                         "FAST_GPU_PAGE_KEEP: page {:#x}: {} GPU-written bytes kept (the {} "
                         "write at {:#x}..{:#x} beside them would have replaced them with the "
                         "guest's stale bytes); {} kept so far",
                         kept.page, kept_bytes, kept.emulator ? "emulator's" : "guest",
                         kept.write_lo, kept.write_hi, n + 1);
            }
        }
    }

    std::unique_ptr<std::array<KeptPage, KeptPages>> kept_pages =
        std::make_unique<std::array<KeptPage, KeptPages>>();
    std::unique_ptr<std::array<KeptBucket, KeptBuckets>> kept_hash =
        std::make_unique<std::array<KeptBucket, KeptBuckets>>();
    std::unique_ptr<std::array<VAddr, KeptPages>> kept_slot_page = // each slot's page (0: free)
        std::make_unique<std::array<VAddr, KeptPages>>();
    std::array<u32, KeptPages> kept_free{AllKeptSlots()}; // free slots (a stack)
    u32 kept_free_count{KeptPages};
    u64 kept_seq{0};
    std::unique_ptr<std::array<KeptOrder, KeptOrderSize>> kept_order =
        std::make_unique<std::array<KeptOrder, KeptOrderSize>>();
    u32 kept_order_head{0};
    u32 kept_order_count{0};
    KeptLock kept_lock;
    std::atomic<u32> kept_count{0};
#ifndef BBVR_NO_LOGS
    // KeptLock's watchdog: a hold past 3 s is logged once (and again every 10 s while it lasts).
    void WatchKeptLock(std::stop_token stop) {
        Common::SetCurrentThreadName("hang:KeptLock");
        s64 reported_since = 0, reported_at = 0;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const u32 owner = kept_lock.owner.load(std::memory_order_relaxed);
            const s64 since = kept_lock.since.load(std::memory_order_relaxed);
            const s64 now = KeptLock::Now();
            if (owner == 0 || now - since < 3'000'000'000) {
                continue;
            }
            if (since == reported_since && now - reported_at < 10'000'000'000) {
                continue;
            }
            reported_since = since;
            reported_at = now;
            LOG_CRITICAL(Render_Vulkan,
                         "FREEZE HUNT: kept_lock held {:.1f} s by thread {}; {} waiting (the "
                         "last: thread {}); {} pages kept",
                         static_cast<double>(now - since) * 1e-9, owner,
                         kept_lock.waiters.load(std::memory_order_relaxed),
                         kept_lock.waiter.load(std::memory_order_relaxed),
                         kept_count.load(std::memory_order_relaxed));
            Common::Log::Flush();
        }
    }
    std::jthread hang_watch; // last: stopped and joined first
#endif
    std::atomic<u64> kept_logs{0};
    std::atomic<u64> kept_evictions{0};

    /// Unlocked bit tests (bits 17/24). FAST_TRACKER_VERIFY=N re-checks one call in N per
    /// thread under the lock and classifies disagreements; sampled calls return the locked answer.
    struct VerifyCounters {
        std::atomic<u64> samples, logic, races, unsafe, safe;
    };
    static VerifyCounters& Counters() noexcept {
        static VerifyCounters counters{};
        return counters;
    }
    // Read once at start-up: Relaxed runs for every region query, and a function-local static's
    // guard is a thread-local read per call.
    static inline const u32 verify_period = [] {
        const char* v = std::getenv("FAST_TRACKER_VERIFY");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? u32(n) : 0u;
    }();
    template <Type type, bool all = false>
    static bool Relaxed(RegionManager* m, u64 offset, size_t size) {
        static_assert(!all || type == Type::GPU);
        const u32 period = verify_period;
        static thread_local u32 tick = 0;
        if (period == 0 || ++tick % period != 0) [[likely]]
            return m->template TestRelaxed<type, all>(offset, size);
        const u64 epoch = m->EpochSum(offset, size);
        const bool fast = m->template TestRelaxed<type, all>(offset, size);
        std::scoped_lock lk{m->lock};
        const bool locked = all ? m->IsRegionAllGpuModified(offset, size)
                                : m->template IsRegionModified<type>(offset, size);
        const bool held = m->template TestRelaxed<type, all>(offset, size);
        auto& c = Counters();
        const u64 n = c.samples.fetch_add(1, std::memory_order_relaxed) + 1;
        if (held != locked) {
            c.logic.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR(Render_Vulkan, "FAST tracker verify LOGIC type={} all={} addr={:#x} size={} relaxed={} locked={}",
                      int(type), all, m->GetCpuAddr() + offset, size, held, locked);
        } else if (fast != locked) {
            if (m->EpochSum(offset, size) != epoch)
                c.races.fetch_add(1, std::memory_order_relaxed);
            else if (fast == all) {
                if (c.unsafe.fetch_add(1, std::memory_order_relaxed) < 32)
                    LOG_WARNING(Render_Vulkan, "FAST tracker verify unsafe type={} all={} addr={:#x} size={} stage_b={}",
                                int(type), all, m->GetCpuAddr() + offset, size, FastRender::IsStageB());
            } else
                c.safe.fetch_add(1, std::memory_order_relaxed);
        }
        if ((n & 65535) == 0)
            LOG_INFO(Render_Vulkan, "FAST tracker verify: samples={} logic={} races={} unsafe={} safe={}",
                     n, c.logic.load(), c.races.load(), c.unsafe.load(), c.safe.load());
        return locked;
    }

    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager = top_tier[page_index].load(std::memory_order_acquire);
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index].load(std::memory_order_acquire);
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index].store(new_manager, std::memory_order_release);
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<std::atomic<RegionManager*>, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
