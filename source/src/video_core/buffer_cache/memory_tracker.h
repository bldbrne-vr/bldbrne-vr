// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
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
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush,
                          bool write_fault = false) noexcept {
        IteratePages<false>(
            cpu_addr, size,
            [this, cpu_addr, write_fault, &on_flush](RegionManager* manager, u64 offset,
                                                     size_t size) {
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
                        KeepGpuPagesAside(manager, offset, size, cpu_addr);
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
    /// counter) uploads the whole page as before, and so does a page the pool (KeptPages) has no
    /// room for. Only guest write faults take part, never the emulator's own writes into guest
    /// memory; those, and a buffer the garbage collector drops, forget the kept pages of their
    /// range (ForgetKept): their next upload is whole again.
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
        for (size_t i = 0; i < KeptPages; ++i) {
            const VAddr page = kept_index[i];
            if (page != 0 && page >= first && page < address + bytes) {
                SetKept((*kept_pages)[i], 0);
                kept_count.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    /// ForEachUploadRange's `func` for [address, address + bytes): calls `emit(address, bytes)`
    /// for what to upload. `gpu_ranges(page, size, f)` calls f(start, end) for each GPU-written
    /// byte range of the page (BufferCache::gpu_modified_ranges). The pages are write-protected
    /// again by then (ForEachModifiedRange), so later guest writes fault anew.
    template <typename Emit, typename GpuRanges>
    void ForUpload(VAddr address, u64 bytes, Emit&& emit, GpuRanges&& gpu_ranges) {
        if (kept_count.load(std::memory_order_acquire) == 0) [[likely]] {
            emit(address, bytes);
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
                emit(plain, lo - plain);
            }
            plain = hi;
            EmitKeptPage(*kept, lo, hi, emit, gpu_ranges);
            SetKept(*kept, 0);
            kept_count.fetch_sub(1, std::memory_order_release);
        }
        if (plain < end) {
            emit(plain, end - plain);
        }
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
    // not allocate). `page` 0: a free slot. kept_index mirrors each slot's page in 2 KB, so a
    // lookup does not touch the 1 MB of kept bytes.
    static constexpr size_t KeptPages = 256;
    struct KeptPage {
        VAddr page{};
        VAddr fault{};
        std::array<u8, TRACKER_BYTES_PER_PAGE> bytes{};
    };

    KeptPage* FindKept(VAddr page) {
        for (size_t i = 0; i < KeptPages; ++i) {
            if (kept_index[i] == page) {
                return &(*kept_pages)[i];
            }
        }
        return nullptr;
    }

    void SetKept(KeptPage& kept, VAddr page) {
        kept.page = page;
        kept_index[&kept - kept_pages->data()] = page;
    }

    // The manager's lock held, before the page's CPU state changes: each GPU-modified page of
    // [offset, offset + size) that is still CPU-clean (write-protected: this is its first write
    // since its last upload) is kept aside as it is now.
    void KeepGpuPagesAside(RegionManager* manager, u64 offset, size_t size, VAddr fault) {
        const u64 first = Common::AlignDown(offset, TRACKER_BYTES_PER_PAGE);
        for (u64 at = first; at < offset + size; at += TRACKER_BYTES_PER_PAGE) {
            if (!manager->template IsRegionModified<Type::GPU>(at, TRACKER_BYTES_PER_PAGE) ||
                manager->template IsRegionModified<Type::CPU>(at, TRACKER_BYTES_PER_PAGE)) {
                continue;
            }
            const VAddr page = manager->GetCpuAddr() + at;
            std::scoped_lock lk{kept_lock};
            KeptPage* kept = FindKept(page);
            if (kept == nullptr) {
                kept = FindKept(0);
                if (kept == nullptr) {
                    continue; // no room: the page uploads whole, as before
                }
                kept_count.fetch_add(1, std::memory_order_release);
            }
            SetKept(*kept, page);
            kept->fault = fault;
            std::memcpy(kept->bytes.data(), reinterpret_cast<const void*>(page),
                        TRACKER_BYTES_PER_PAGE);
        }
    }

    // ForUpload, kept_lock held: [lo, hi) of a kept page.
    template <typename Emit, typename GpuRanges>
    void EmitKeptPage(const KeptPage& kept, VAddr lo, VAddr hi, Emit& emit,
                      GpuRanges& gpu_ranges) {
        std::array<std::pair<VAddr, VAddr>, 16> ranges{};
        size_t count = 0;
        bool fault_inside = false;
        bool overflow = false;
        gpu_ranges(kept.page, TRACKER_BYTES_PER_PAGE, [&](VAddr start, VAddr stop) {
            fault_inside = fault_inside || (kept.fault < stop && kept.fault + 8 > start);
            if (count < ranges.size()) {
                ranges[count++] = {start, stop};
            } else {
                overflow = true;
            }
        });
        if (count == 0 || fault_inside || overflow) {
            emit(lo, hi - lo); // nothing the GPU wrote, or the CPU rewrites it: as before
            return;
        }
        const u8* guest = reinterpret_cast<const u8*>(kept.page);
        u64 kept_bytes = 0;
        VAddr cursor = lo;
        for (size_t r = 0; r < count; ++r) {
            const VAddr start = std::clamp(ranges[r].first, lo, hi);
            const VAddr stop = std::clamp(ranges[r].second, lo, hi);
            if (start >= stop || start < cursor) {
                continue;
            }
            if (cursor < start) {
                emit(cursor, start - cursor); // outside the GPU-written bytes: as before
            }
            // A GPU-written range the CPU changed at all uploads whole; one it left alone keeps
            // the GPU's bytes.
            if (std::memcmp(guest + (start - kept.page), kept.bytes.data() + (start - kept.page),
                            stop - start) != 0) {
                emit(start, stop - start);
            } else {
                kept_bytes += stop - start;
            }
            cursor = stop;
        }
        if (cursor < hi) {
            emit(cursor, hi - cursor);
        }
        if (kept_bytes != 0) {
            const u64 n = kept_logs.fetch_add(1, std::memory_order_relaxed);
            if (n < 64 || (n & 1023) == 0) {
                LOG_INFO(Render_Vulkan,
                         "FAST_GPU_PAGE_KEEP: page {:#x}: {} GPU-written bytes kept (the guest "
                         "write at {:#x} beside them would have replaced them with the guest's "
                         "stale bytes); {} kept so far",
                         kept.page, kept_bytes, kept.fault, n + 1);
            }
        }
    }

    std::unique_ptr<std::array<KeptPage, KeptPages>> kept_pages =
        std::make_unique<std::array<KeptPage, KeptPages>>();
    std::array<VAddr, KeptPages> kept_index{};
    std::mutex kept_lock;
    std::atomic<u32> kept_count{0};
    std::atomic<u64> kept_logs{0};

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
