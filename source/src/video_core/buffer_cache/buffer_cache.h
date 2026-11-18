// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <deque>
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
#include "video_core/texture_cache/memory_policy.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/sparse_pool.h"
#include "video_core/buffer_cache/upload_staging.h"
#include "video_core/fast_render.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/types.h"

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
struct VertexBinds;
class RecorderProxy;
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

/// The elements a direct non-indexed draw fetches (BindVertexBuffers' vertex stream): vertices
/// [first_vertex, first_vertex + vertex_count) of each per-vertex buffer, instances
/// [first_instance, first_instance + instance_count) of each per-instance one: the draw's own
/// vkCmdDraw arguments.
struct VertexFetch {
    u32 first_vertex = 0;
    u32 vertex_count = 0;
    u32 first_instance = 0;
    u32 instance_count = 0;
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

    /// A memo outside the cache binds this buffer again (perf_copy_memo): its idle age restarts.
    void MarkUsed(BufferId id) {
        NoteUsed(id);
    }

    /// Retrieves a utility buffer optimized for specified memory usage.
    StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept {
        if (usage == MemoryUsage::Stream) {
            return stream_buffer;
        } else if (usage == MemoryUsage::Download) {
            return download_buffer;
        } else {
            ASSERT(usage == MemoryUsage::DeviceLocal);
            return device_buffer;
        }
    }

    /// Invalidates any buffer in the logical page range. `emulator_write`: the emulator writes
    /// exactly these bytes (a file read; an unmap is not one), FAST_GPU_PAGE_KEEP keeps the GPU's
    /// bytes beside them.
    void InvalidateMemory(VAddr device_addr, u64 size, bool emulator_write = false,
                          bool protection_only = false);

    /// The game handed [addr, addr + size) of GPU memory out again (any thread): what the GPU
    /// wrote there is dead. Its kept pages (FAST_GPU_PAGE_KEEP) are forgotten and the pages wholly
    /// inside stop being GPU-modified now; the GPU-written byte ranges inside go on the recording
    /// thread before its next upload or GPU write (TakeFresh).
    void NoteFreshRange(VAddr addr, u64 size);

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
    /// Empty while a snapshot's GPU copy is still in flight (the draw keeps camera motion;
    /// VR_MOTION_READBACK_WAIT=1 waits for the copy instead, as before 2026-11-07).
    /// `frame`: the caller's frame number. A snapshot used in this frame is never evicted for
    /// another request: with more requests than room, that request keeps camera motion.
    [[nodiscard]] std::span<const u8> ReadForMotion(VAddr address, u32 size, u64 frame);

    /// A write into [address, address + size) of a cached buffer is recorded now: the motion
    /// snapshots holding any of its bytes are stale. Every write the cache records into its
    /// buffers calls it (uploads, writes bound by ObtainBuffer, resyncs, data writes, fills),
    /// and so does a write bound without the cache (perf_copy_memo's hits). Recording thread;
    /// a plain return while no snapshot is held.
    void NoteMotionWrite(VAddr address, u64 size) {
        if (!motion_snapshots.empty()) [[unlikely]] {
            MarkMotionWrite(address, size);
        }
    }

    /// ReadForMotion's answers since the last call (the VR temporal frame log).
    struct MotionReadCounts {
        u32 snapshots{}; // a GPU snapshot ready for this exact request
        u32 waiting{};   // its copy still in flight: camera motion for this draw
        u32 copies{};    // a new GPU copy asked for (recorded at the next submit)
        u32 replaced{};  // of which an older snapshot of the request was stale
        u32 no_room{};   // no copy: the motion staging ring is full (camera motion, asked again)
        u32 full{};      // no copy: every snapshot held was used this frame (camera motion)
    };
    [[nodiscard]] MotionReadCounts TakeMotionReadCounts() noexcept {
        return std::exchange(motion_reads, {});
    }

    static void PrepareVertexInputs(const Vulkan::GraphicsPipeline& pipeline,
                                    u32 step_rate_0, u32 step_rate_1,
                                    Vulkan::VertexPreparation& output);

    /// Binds host vertex buffers for the current draw. `prepared`: stage A's vertex preparation
    /// of this draw (FastRender bit 14), or null to prepare it here. `fetch`: what the draw
    /// fetches when it is a direct non-indexed draw (the vertex stream), else null.
    void BindVertexBuffers(const Vulkan::GraphicsPipeline& pipeline,
                           boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
                           u64* motion_geometry = nullptr,
                           const Vulkan::VertexView* prepared = nullptr,
                           Vulkan::VertexBinds* binds = nullptr,
                           const VertexFetch* fetch = nullptr);

    /// Bind host index buffer for the current draw.
    void BindIndexBuffer(u32 index_offset,
                         boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
                         Vulkan::VertexBinds* binds = nullptr);
    /// FAST_CPU_STAGEB_SPLIT: encodes what BindVertexBuffers / BindIndexBuffer filled in `binds`.
    static void EncodeVertexBinds(const Vulkan::VertexBinds& binds,
                                  const Vulkan::RecorderProxy& cmdbuf);

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
    struct ImageUploadBuffer {
        Buffer* buffer;
        u32 offset;
        // The consumer retires staging after recording detiling/image upload commands.
        std::optional<UploadStagingPool::Allocation> staging;
    };
    [[nodiscard]] ImageUploadBuffer ObtainBufferForImage(VAddr gpu_addr, u32 size);
    /// The same for an image whose bytes are not the guest's own (texture_cache/bump_height.h: a
    /// bump map with a height made for its blue): `size` bytes of `data` through the staging pool.
    [[nodiscard]] ImageUploadBuffer ObtainBufferForBytes(const u8* data, u32 size);

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

    /// VR_RETILE_DEFER (on by default; '0' off, 'm' forced at every frame end; read once): a game copy
    /// shader's complete image -> buffer copy (the 3d5ebf4e direct path,
    /// Rasterizer::IsComputeImageCopy) leaves its re-tile pending here instead of recording it.
    /// Its buffer state (ObtainBuffer as written, the images it overlaps invalidated) is as if
    /// recorded. The re-tile is recorded first when anything asks for those bytes: every
    /// BufferCache entry point that binds, copies, fills, downloads, uploads from or synchronizes
    /// guest memory, the vertex and HLE copy memos, raw-memory (DMA) shaders, and the source
    /// image's unregistration. A later copy of the same range drops the pending one (it rewrites
    /// every byte); an overlapping one records it first. Reading late (the source drawn again
    /// since), the bytes are the source's current frame; a destination read once is never
    /// deferred again ('VR retile defer:' warning; a fifth such destination turns deferring off).
    /// Off with GPU readbacks, linear image readbacks or copy threads. Recording thread only
    /// (stage B, or A with B drained).
    bool DeferImageRetile(VAddr dst, u64 bytes, ImageId source, const vk::BufferImageCopy& region);
    /// Records every pending re-tile overlapping [addr, addr + size) (nothing pending: one load).
    void MaterializeRetiles(VAddr addr, u64 size, u32 trigger) {
        if (retile_count != 0) [[unlikely]] {
            MaterializeRetilesIn(addr, size, trigger);
        }
        if (copy_sink_hook) [[unlikely]] {
            copy_sink_hook(addr, size);
        }
    }
    /// The copy sink (vk_shader_hle.cpp CopySink), while copies wait for the end of the open pass:
    /// every access above, which records them first if it meets their bytes. Null: nothing waits.
    /// Every entry point that touches guest buffer memory on the GPU calls MaterializeRetiles first,
    /// so a new one must too.
    static inline void (*copy_sink_hook)(VAddr addr, u64 size) = nullptr;
    void MaterializeAllRetiles(u32 trigger) {
        if (retile_count != 0) [[unlikely]] {
            MaterializeRetilesIn(0, ~u64{0}, trigger);
        }
    }
    /// The source image is going away (TextureCache::UnregisterImage): its pending re-tiles first.
    void MaterializeRetilesFrom(ImageId image);
    /// TextureCache::ReclaimMemoryFor never deletes a pending re-tile's source.
    [[nodiscard]] bool IsRetileSource(ImageId image) const noexcept;
    /// VR_RETILE_DEFER=m: every pending re-tile recorded (Rasterizer::FlushFrameEnd).
    void RetileFrameEnd();
    /// MaterializeRetiles' trigger, for the log.
    enum RetileTrigger : u32 {
        RetileObtain, RetileCopy, RetileFill, RetileDownload, RetileImageUpload, RetileIndex,
        RetileMotion, RetileRange, RetileDma, RetileVertex, RetileHleCopy, RetileSourceGone,
        RetileOverlap, RetileForced, NumRetileTriggers
    };

    /// Return buffer id for the specified region
    BufferId FindBuffer(VAddr device_addr, u32 size);
    // Stamps the buffer's last_used_tick with gc_tick; returns the id.
    BufferId NoteUsed(BufferId buffer_id);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers in the specified range.
    void SynchronizeBuffersInRange(VAddr device_addr, u64 size);

    /// Synchronizes all buffers neede for DMA.
    void SynchronizeDmaBuffers();

    /// Runs the garbage collector.
    /// `transition_request`: VR::TakeTransitionTextureCleanupRequest (a warp, death or new map).
    void RunGarbageCollector(u64 transition_request = 0);
    /// Frames so far (RunGarbageCollector calls).
    [[nodiscard]] u64 GcTick() const noexcept {
        return gc_tick;
    }
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

    /// True when both are sparse views: the new buffer took the old one's pieces, no copy
    /// (CreateBuffer then orders the old handle's writes before the new one's uses).
    bool JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);
    /// FAST_SPARSE_VERIFY (a test): the merged old view's bytes against the new view's at
    /// `offset` (the same pieces), read back and compared once the GPU is done.
    void VerifySparseMerge(vk::Buffer old_view, u64 bytes, vk::Buffer new_view, u64 offset);

    BufferId CreateBuffer(VAddr device_addr, u32 wanted_size);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    bool SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer, bool retire_cpu_uploads = false);

    std::optional<UploadStagingPool::Allocation> UploadCopies(
        Buffer& buffer, std::span<vk::BufferCopy> copies, size_t total_size_bytes,
        bool async_upload = false);

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

    /// NoteFreshRange's ranges, taken on the recording thread: one relaxed check when none wait.
    void TakeFresh() {
        if (fresh_pending.load(std::memory_order_acquire)) [[unlikely]] {
            ProcessFresh();
        }
    }
    void ProcessFresh();
    std::mutex fresh_mutex;
    std::vector<std::pair<VAddr, u64>> fresh_ranges; // fresh_mutex
    std::atomic<bool> fresh_pending{false};

    /// Under VRAM pressure, buffers unused for BufferEviction::IdleSeconds whose bytes the game's
    /// memory holds (buffer_eviction.h); once a second at most.
    void EvictIdleBuffers(bool critical, u64 second);
    /// The eviction marks from a budget (the start's, or a load's revision).
    void SetMemoryMarks(s64 budget);
    /// At a load: the marks follow Windows' budget for the game now (TextureMemory::RevisedBudget).
    /// `live`: a reading during play (LiveBudget), applied when confirmed; else a load's.
    void ReviseMemoryMarks(bool live = false);
    /// Clean buffers unused since `cutoff` (a gc_tick), biggest first, within the bounds.
    void EvictUnusedSince(u64 cutoff, std::size_t max_count, u64 max_bytes, bool transition);
    TextureMemory::TransitionCleanup transition_cleanup;
    TextureMemory::LiveBudget live_budget;
    TextureMemory::SpillWatch spill_watch;
    bool spill_was_active{};
    u32 spill_readings{};
    u64 spill_sample{}; // the last Windows reading the watch took
    void VerifyEviction(Buffer& buffer); // FAST_BUFFER_EVICT_VERIFY
    TextureMemory::AgeCutoff gc_seconds; // gc_tick at the start of each second
    u64 evict_second{};
    u64 evicted_buffers{}, evicted_bytes{}, evict_logged_second{};
    u64 released_twins{}, released_twin_bytes{};

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    FaultManager fault_manager;
    std::unique_ptr<MemoryTracker> memory_tracker;
    UploadStagingPool staging_pool;
    StreamBuffer stream_buffer;
    StreamBuffer download_buffer;
    // ReadForMotion's copies: a ring of its own, never through StreamBuffer::Map (whose ring
    // hands bytes out again once the GPU is done, before a deferred copy out of them has run).
    StreamBuffer motion_staging;
    StreamBuffer device_buffer;
    Buffer gds_buffer;
    Buffer bda_pagetable_buffer;
    // Before slot_buffers: destroyed after them, so no view outlives the slabs it binds.
    SparsePool sparse_pool;

public:
    /// FAST_BUFFER_ARENA: the cache's buffers are sparse views (Rasterizer's teardown drains).
    [[nodiscard]] bool SparseEnabled() const noexcept {
        return sparse_pool.Enabled();
    }

private:
    Common::SlotVector<Buffer> slot_buffers;
    // A snapshot's bytes, filled by the deferred operation of its GPU copy (any thread), shared
    // with it so an evicted snapshot's copy can still land.
    struct MotionBytes {
        std::vector<u8> bytes;
        std::atomic<bool> ready{};
        bool dropped{}; // recording thread: its copy was never recorded (its staging is free)
    };
    // Pending: its copy goes out with the next submit (FlushMotionCopies). Copied: recorded, lands
    // with that submit. Stale: a write into its bytes was recorded (before the copy or after it);
    // the request's next ReadForMotion replaces it.
    enum class MotionState : u8 { Pending, Copied, Stale };
    struct MotionSnapshot {
        VAddr address{};
        u32 size{};
        MotionState state{};
        u64 serial{}, last_use{}, last_frame{};
        std::shared_ptr<MotionBytes> data;
    };
    // Sorted by (address, size), one per request. Only ReadForMotion erases one: the view it
    // handed out lives until its next call, whatever a write or a submit marks meanwhile.
    std::vector<MotionSnapshot> motion_snapshots;
    u64 motion_snapshot_use{}, motion_snapshot_bytes{};
    u32 motion_snapshot_longest{}; // the longest request held: NoteMotionWrite's search window
    MotionReadCounts motion_reads{};
    // The copies asked for since the last submit, in order, recorded together before it.
    struct MotionCopy {
        VAddr address{};
        u32 size{};
        BufferId buffer_id{};
        u64 serial{};
        vk::Buffer staging{};
        vk::BufferCopy region{};
        u64 prefix{}; // the request's first byte past region.dstOffset (dword-aligned copies)
        u8* host{};   // region.dstOffset in the staging buffer's mapping
        bool coherent{};
        VmaAllocation allocation{};
        std::shared_ptr<MotionBytes> data;
    };
    std::vector<MotionCopy> motion_copies;
    // The motion staging ring's copies, oldest first: the bytes each holds (with a tail skipped
    // before the ring starts over) and its snapshot, whose `ready` (or `dropped`) gives them back.
    struct MotionStagingUse {
        u64 bytes{};
        std::shared_ptr<MotionBytes> data;
    };
    std::deque<MotionStagingUse> motion_staging_uses;
    u64 motion_staging_head{}, motion_staging_used{};
    /// The first snapshot not ordered before (address, size): where that request's is, or goes.
    std::vector<MotionSnapshot>::iterator MotionSnapshotBound(VAddr address, u32 size);
    void MarkMotionWrite(VAddr address, u64 size);
    /// Records every copy asked for since the last submit (one barrier before them, one after)
    /// and returns what lands them, once the GPU is done with this tick.
    [[nodiscard]] Common::UniqueFunction<void> RecordMotionCopies();
    /// Before every submit (the pre-submit hook): RecordMotionCopies, landed by a deferred
    /// operation of the tick it submits.
    void FlushMotionCopies();
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
    s64 start_budget = 0;     // the budget at the start (0: none to follow, or a test's card)
    s64 marks_budget = 0;     // the one the marks come from now
    u64 revised_sequence = 0; // the last load (transition request) the marks were revised at
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

    // VR_RETILE_DEFER (DeferImageRetile): the pending re-tiles, recording thread only.
    struct DeferredRetile {
        VAddr lo{}, hi{};
        ImageId image{};
        u64 image_uid{};
        u64 write_serial{}; // the source's at the copy (Image::write_serial)
        vk::BufferImageCopy region{};
        bool valid{};
    };
    std::array<DeferredRetile, 4> retiles{};
    u32 retile_count{};
    bool retile_off{}; // readers of too many destinations: no more deferring this session
    // Destinations a reader asked for: their copies are recorded when made from then on.
    std::array<std::pair<VAddr, VAddr>, 4> retile_read{};
    u32 retile_read_count{};
    std::array<u64, NumRetileTriggers> retile_triggers{};
    u64 retile_deferred{}, retile_superseded{}, retile_materialized{};
    u64 retile_late{}; // recorded after its source changed: the source's newer bytes
    void MaterializeRetilesIn(VAddr addr, u64 size, u32 trigger);
    void MaterializeRetile(DeferredRetile& retile, u32 trigger);
    void ReportRetiles();
};

} // namespace VideoCore
