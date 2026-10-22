// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <utility>
#include <boost/container/small_vector.hpp>
#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/fast_render.h"
#include "video_core/multi_level_page_table.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct VertexPreparation;
struct VertexView;
}

namespace VideoCore {

using BufferId = Common::SlotId;

class TextureCache;
struct Image;
class MemoryTracker;
class PageManager;
struct FaultWindowCounts;

/// FastRender bit 33 (BufferCache::ObtainBuffer): passed by a caller that can take a small
/// read-only range bound in place from its cached buffer instead of copied into the stream
/// buffer. shader_read: the caller asks the buffer for a shader-read barrier (BindBuffers), so
/// only a buffer already in that state is taken (no barrier, no render-pass break); with bit 44
/// (SmallReadSettle) also one whose writes are all visible to shader reads, set to that state
/// without a barrier. direct: set when it was taken; the range was then found not GPU-modified,
/// as for the stream copy.
struct SmallRead {
    bool shader_read = false;
    bool direct = false;
};

class BufferCache {
public:
    /// FastRender bit 33: small read-only ranges in CPU-clean pages of an existing cached buffer
    /// are bound there (no stream copy). Decided per draw (SetSmallReadDirect).
    static constexpr auto SmallReadDirect = static_cast<FastRender::Bit>(33);
    /// FastRender bit 44 (needs bit 33): a shader read's buffer whose recorded barrier state is
    /// not shader read at every stage is still bound in place when every write of it is already
    /// visible to shader reads (ObtainSmallDirect, Settle). Read per binding.
    static constexpr auto SmallReadSettle = static_cast<FastRender::Bit>(44);

    static constexpr u32 CACHING_PAGEBITS = 14;
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;
    static constexpr u64 DEVICE_PAGESIZE = 16_KB;
    static constexpr u64 CACHING_NUMPAGES = u64{1} << (40 - CACHING_PAGEBITS);
    static constexpr u64 BDA_PAGETABLE_SIZE = CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

    // Default values for garbage collection
    static constexpr s64 DEFAULT_TRIGGER_GC_MEMORY = 1_GB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 2_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    struct PageData {
        BufferId buffer_id{};
    };

    struct Traits {
        using Entry = PageData;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 16;
        static constexpr size_t PageBits = CACHING_PAGEBITS;
    };
    using PageTable = MultiLevelPageTable<Traits>;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        VAddr begin;
        VAddr end;
        bool has_stream_leap = false;
    };

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         AmdGpu::Liverpool* liverpool, TextureCache& texture_cache,
                         PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return &bda_pagetable_buffer;
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager.GetFaultBuffer();
    }

    /// Retrieves the buffer with the specified id.
    [[nodiscard]] Buffer& GetBuffer(BufferId id) {
        return slot_buffers[id];
    }

    /// Retrieves a utility buffer optimized for specified memory usage.
    StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept {
        if (usage == MemoryUsage::Stream) {
            return stream_buffer;
        } else if (usage == MemoryUsage::Download) {
            return download_buffer;
        } else if (usage == MemoryUsage::DeviceLocal) {
            return device_buffer;
        } else {
            return staging_buffer;
        }
    }

    /// Invalidates any buffer in the logical page range. `emulator_write`: the emulator writes
    /// exactly these bytes (a file read; an unmap is not one), FAST_GPU_PAGE_KEEP keeps the GPU's
    /// bytes beside them.
    void InvalidateMemory(VAddr device_addr, u64 size, bool emulator_write = false,
                          bool protection_only = false);

    /// InvalidateMemory for a guest write fault there, before the write lands
    /// (FAST_GPU_PAGE_KEEP: MemoryTracker::KeepGpuPages).
    void InvalidateWriteFault(VAddr device_addr, u64 size);

    /// Fast renderer: lifts the write protection of the CPU-clean pages around a guest write
    /// fault; GPU-modified ones are kept only with readbacks on (see
    /// MemoryTracker::MarkCleanPagesCpuModified).
    void UnprotectCleanWindow(VAddr device_addr, u64 size);

    /// FAST_CPU_FAULT_WINDOW: the same pages in one protection change per region
    /// (RegionManager::MarkFaultWindow). `counts`: verify only.
    void MarkFaultWindow(VAddr device_addr, u64 size, FaultWindowCounts* counts = nullptr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false);

    /// Read-only motion indices: CPU data directly, or a bounded cached GPU snapshot.
    /// Recording thread, before draw binding. Never writes guest memory; the returned
    /// view is consumed by this draw and must not outlive the next ReadForMotion call.
    [[nodiscard]] std::span<const u8> ReadForMotion(VAddr address, u32 size);

    static void PrepareVertexInputs(const Vulkan::GraphicsPipeline& pipeline,
                                    u32 step_rate_0, u32 step_rate_1,
                                    Vulkan::VertexPreparation& output);

    /// Binds host vertex buffers for the current draw. `prepared`: stage A's vertex preparation
    /// of this draw (FastRender bit 14), or null to prepare it here.
    void BindVertexBuffers(const Vulkan::GraphicsPipeline& pipeline,
                           boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
                           u64* motion_geometry = nullptr,
                           const Vulkan::VertexView* prepared = nullptr);

    /// Bind host index buffer for the current draw.
    void BindIndexBuffer(u32 index_offset,
                         boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers);

    /// Writes a value to GPU buffer. (uses command buffer to temporarily store the data)
    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);

    /// Performs buffer to buffer data copy on the GPU.
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);

    /// Obtains a buffer for the specified region. small_read: see SmallRead (bit 33).
    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(VAddr gpu_addr, u32 size, bool is_written,
                                                       bool is_texel_buffer = false,
                                                       BufferId buffer_id = {},
                                                       SmallRead* small_read = nullptr);

    /// FastRender bit 33, set by Rasterizer::BindResources for each draw or dispatch: whether
    /// ObtainBuffer may bind small read-only ranges in place for it (the bit is on and no stage
    /// writes buffers or raw memory). Recording thread only.
    void SetSmallReadDirect(bool allowed) noexcept {
        small_read_direct = allowed && small_direct_supported;
    }

    /// FAST_CPU_GUARD_VERIFY (Rasterizer::CheckGuardConstants): whether bit 33 could bind this
    /// small read in its cached buffer (ObtainSmallDirect's tests without the per-draw permission
    /// and the barrier state, which only refuse more): a buffer holds it, not image data, and
    /// every page is CPU-clean. Its bytes are then the buffer's, from an upload a write through
    /// the backing (a label, a download) does not reach. Not GPU-modified is the caller's test.
    /// Recording thread only.
    [[nodiscard]] bool SmallDirectCandidate(VAddr device_addr, u32 size);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<Buffer*, u32> ObtainBufferForImage(VAddr gpu_addr, u32 size);

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(VAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Exact dirty-byte intersection, unlike the page-granularity region query.
    [[nodiscard]] bool HasGpuWrites(VAddr addr, size_t size) const;

    /// perf_copy_memo (vk_shader_hle.cpp): bumped by every buffer registered or unregistered (the
    /// buffers ObtainBuffer finds) and every GPU-modified range dropped by a download
    /// (gpu_modified_ranges). While it and a range's tracking epoch are unchanged, ObtainBuffer
    /// of that range finds the same buffer and has nothing to upload, mark or add.
    [[nodiscard]] u64 CopyMemoEpoch() const noexcept {
        return copy_memo_epoch.load(std::memory_order_acquire);
    }

    /// The range's write-tracking epoch (MemoryTracker::RangeEpoch; 0 = not tracked yet).
    [[nodiscard]] u64 TrackingEpoch(VAddr addr, u64 size) const noexcept;

    /// The buffer registered at the address, if any (no creation).
    [[nodiscard]] BufferId BufferIdAt(VAddr addr) noexcept {
        return page_table[addr >> CACHING_PAGEBITS].buffer_id;
    }

    /// perf_copy_memo's check: every page of the range is marked GPU-modified and the range is
    /// in the GPU-modified range set (ObtainBuffer of it as written would add nothing).
    [[nodiscard]] bool IsRangeFullyGpuModified(VAddr addr, u64 size);

    /// Return buffer id for the specified region
    BufferId FindBuffer(VAddr device_addr, u32 size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers in the specified range.
    void SynchronizeBuffersInRange(VAddr device_addr, u64 size);

    /// Synchronizes all buffers neede for DMA.
    void SynchronizeDmaBuffers();

    /// Runs the garbage collector.
    void RunGarbageCollector();
    /// Experiment: merged stream buffers grow geometrically (VR_BUFFER_GROW_FILE, logs-on builds).
    static bool BufferGrowGeometric();
    static void PollBufferGrowSwitch();
    // Diagnostics (the VR_DRAW_CTL inspector's gpumem): the GPU's bytes of the range copied back
    // to guest memory at the frame's end (with readbacks off they never are otherwise).
    void RequestReadback(VAddr addr, u64 size);

private:
    template <typename Func>
    void ForEachBufferInRange(VAddr device_addr, u64 size, Func&& func) {
        buffer_ranges.ForEachInRange(device_addr, size,
                                     [&](u64 page_start, u64 page_end, BufferId id) {
                                         Buffer& buffer = slot_buffers[id];
                                         func(id, buffer);
                                     });
    }

    inline bool IsBufferInvalid(BufferId buffer_id) const {
        return !buffer_id || slot_buffers[buffer_id].is_deleted;
    }

    template <bool async>
    void DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size);

    [[nodiscard]] OverlapResult ResolveOverlaps(VAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    BufferId CreateBuffer(VAddr device_addr, u32 wanted_size);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    bool SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer, bool retire_cpu_uploads = false);

    vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                            size_t total_size_bytes, bool async_upload = false);

    bool SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size);

    /// FastRender bit 33: the existing cached buffer of a small read-only range that is not
    /// GPU-modified, when it can be bound in place (ObtainBuffer); nothing is created or uploaded.
    std::optional<std::pair<Buffer*, u32>> ObtainSmallDirect(VAddr device_addr, u32 size,
                                                             BufferId buffer_id, bool shader_read);

    /// FAST_SMALL_READ_DIRECT_VERIFY: the bytes a sampled direct binding would read (both eyes
    /// when a descriptor pairs a right copy) read back and compared with guest memory. by_settle:
    /// the binding was bit 44's (counted apart).
    void VerifySmallDirect(Buffer& buffer, VAddr device_addr, u32 size, bool both_eyes,
                           bool by_settle);

    /// VR_PERF_CHECK: a resync the memo skipped, compared with a full one (vr_perf.h).
    void CheckSkippedResync(Image& image, std::span<vk::BufferImageCopy> buffer_copies,
                            Buffer& buffer, u32 buf_offset, u32 copy_size);

    /// FAST bit 34 (buffer_cache.cpp): the index range's buffer without the stream copy, or
    /// nothing when BindIndexBuffer's own path must run.
    struct IndexBindMemo;
    std::optional<std::pair<Buffer*, u32>> ObtainIndexBuffer(
        VAddr address, u32 size,
        boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers);
    bool VerifyIndexHit(IndexBindMemo& memo, BufferId buffer_id, VAddr address, u32 size,
                        u64 epoch, bool gpu_modified);

    void WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes);

    void TouchBuffer(const Buffer& buffer);

    void DeleteBuffer(BufferId buffer_id);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    FaultManager fault_manager;
    std::unique_ptr<MemoryTracker> memory_tracker;
    StreamBuffer staging_buffer;
    StreamBuffer stream_buffer;
    StreamBuffer download_buffer;
    StreamBuffer device_buffer;
    Buffer gds_buffer;
    Buffer bda_pagetable_buffer;
    Common::SlotVector<Buffer> slot_buffers;
    struct MotionSnapshot {
        VAddr address{};
        u64 serial{}, write_seq{}, last_use{};
        std::vector<u8> bytes;
    };
    std::vector<MotionSnapshot> motion_snapshots;
    u64 motion_snapshot_use{}, motion_snapshot_bytes{};
    u64 total_used_memory = 0;
    // FAST_GPU_PAGE_KEEP's catch-all log (NoteGpuBytesReplaced): each page's replacements.
    // `why`: MemoryTracker::LostWhy. Pages seen: an open-addressed table (no lock, no
    // allocation; a full table logs every 64th replacement only).
    void NoteGpuBytesReplaced(VAddr start, VAddr stop, u8 why);
    static constexpr size_t ReplacedSlots = 1 << 16;
    std::unique_ptr<std::array<std::atomic<u64>, ReplacedSlots>> replaced_pages{
        std::make_unique<std::array<std::atomic<u64>, ReplacedSlots>>()};
    std::unique_ptr<std::array<std::atomic<u32>, ReplacedSlots>> replaced_counts{
        std::make_unique<std::array<std::atomic<u32>, ReplacedSlots>>()};
    std::atomic<u64> replaced_total{0};
    std::chrono::steady_clock::time_point replaced_t0{std::chrono::steady_clock::now()};
    u64 trigger_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 gc_tick = 0;
    // FAST_GPU_SETTLE (off unless '1'): a range a shader or a copy wrote that the GPU then leaves
    // alone is copied back to guest memory once (SettleGpuWrites, at the frame's end). With
    // readbacks off the game's copy of GPU-made data (FaceGen morphs, written once per load)
    // stays stale, and any later upload of its page (a write fault beside it, an emulator write,
    // an eviction) put the stale bytes back: faces gone, eyes left. Settled, the game's copy is
    // the GPU's. Data rewritten every frame never settles, so it is never copied.
    struct SettleWrite {
        u64 size{};
        u64 tick{};
    };
    std::unordered_map<VAddr, SettleWrite> settle_writes;
    std::mutex settle_mutex;
    u64 settle_logs{};
    std::vector<std::pair<VAddr, u64>> readback_requests; // settle_mutex
    void NoteGpuWrite(VAddr addr, u64 size);
    void SettleGpuWrites();
    Common::LeastRecentlyUsedCache<BufferId, u64> lru_cache;
    RangeSet gpu_modified_ranges;
    SplitRangeMap<BufferId> buffer_ranges;
    // buffer_ranges is changed on the GPU thread (Register / Unregister) but read from any thread
    // (IsRegionRegistered: InvalidateMemory on a game thread's file read, unmap or write fault).
    // Unsynchronised, a read during a change could follow a half-linked tree node (2026-10-09: a
    // crash while loading, 0xc0000005 reading a null node's flag in IsRegionRegistered).
    std::shared_mutex buffer_ranges_mutex;
    PageTable page_table;
    std::atomic<u64> copy_memo_epoch{1}; // CopyMemoEpoch
    std::unique_ptr<IndexBindMemo> index_bind_memo; // FAST bit 34, recording thread only
    bool small_read_direct{};                       // bit 33, per draw (SetSmallReadDirect)
    bool small_direct_supported{};                  // bit 33 possible (constructor)
    std::shared_ptr<Buffer> small_direct_readback;  // FAST_SMALL_READ_DIRECT_VERIFY
};

} // namespace VideoCore
