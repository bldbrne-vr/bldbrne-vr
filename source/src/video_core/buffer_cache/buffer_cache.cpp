#include <limits>
#include <chrono>
#include <cstdio>
#include "video_core/host_copies.h"
#include <utility>
#include "video_core/native_stereo/draw_registers.h"
#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/fast/b_handler_profile.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <magic_enum/magic_enum.hpp>
#include "common/alignment.h"
#include "common/slow_scope.h"
#include "common/memory_write_trace.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/buffer_eviction.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_transfer_eye_barriers.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/renderer_vulkan/vr_perf_probe.h"
#include "video_core/texture_cache/texture_cache.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <vk_mem_alloc.h>
#include <xxhash.h>
#include "video_core/buffer_cache/dma_table.h"
#include "video_core/vram_report.h"
#include <array>
#include <chrono>

namespace VideoCore {

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t MotionStagingSize = 8_MB; // ReadForMotion's copies in flight
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;

// dma_table.h: the page table in full only with direct memory access on (decided once, here).
static u64 BdaPageTableSize() {
    const bool full = EmulatorSettings.IsDirectMemoryAccessEnabled();
    DmaTable::allocated.store(full, std::memory_order_release);
    return full ? BufferCache::BDA_PAGETABLE_SIZE : 64_KB;
}

/// FAST bit 34: index buffer binds without the per-draw copy (BindIndexBuffer, ObtainIndexBuffer).
/// An index range of 16 KB or less is copied into the stream buffer on every bind (ObtainBuffer's
/// SmallReadStream: Map, CopySparseMemory under the memory lock, Commit, and later the shadow
/// flush). With the bit, a small range bound a second time is synchronized once into the buffer
/// cache instead (FindBuffer + SynchronizeBuffer: the path every larger index range takes), which
/// write-protects it. From then on, while no page of it has been written by the CPU, the
/// registered buffer already holds its bytes and is bound as it is: one page-table lookup and two
/// page-bit tests (unlocked; the GPU one with bit 17). Any index range already CPU-clean in one
/// registered buffer (over 16 KB, or inside a synchronized vertex range) is bound that way too,
/// without FindBuffer / SynchronizeBuffer. A promoted range found written again goes back to the
/// copy (Volatile), so data the game rewrites costs one write fault, not an upload and a render
/// pass break per rewrite. The table is direct-mapped by range like bbport's in-place read memo
/// (bbport buffer_cache.cpp:1084-1112, used by ResolveIndexBuffer, vk_rasterizer.cpp:2718-2749);
/// it keeps only hints, every hit is checked against the live tracker, so the bit may flip at any
/// packet. FAST_INDEX_BIND_PROMOTE=N: promotions per command buffer (8; 0 never promotes).
/// FAST_INDEX_BIND_VERIFY=N: one bind in N, when it hits, is checked against the old path's
/// locked queries and the guest bytes' hash (LOG_ERROR on a mismatch; that bind takes the old
/// path).
static constexpr auto IndexBind = static_cast<FastRender::Bit>(34);

struct BufferCache::IndexBindMemo {
    enum State : u8 { Seen, Promoted, Volatile };
    struct Entry {
        VAddr address;
        u32 size;
        State state;
        u64 verify_epoch; // FAST_INDEX_BIND_VERIFY: the range's tracking epoch when hashed
        u64 verify_hash;  // and the hash of its guest bytes then
    };
    static constexpr u32 SlotBits = 13;
    static constexpr u64 MaxVerifyBytes = 1_MB;
    static size_t Index(VAddr address, u32 size) noexcept {
        return static_cast<size_t>(((address ^ (u64{size} << 40)) * 0x9E3779B97F4A7C15ULL) >>
                                   (64 - SlotBits));
    }
    std::array<Entry, size_t{1} << SlotBits> entries;
    u64 tick;
    u32 budget;
    u32 verify_count;
    u64 calls, hits, misses, promotions, demotions, verified, races, mismatches;
};

namespace {
// The environment switches of this file's per-draw paths, read once at start-up: a plain load
// per use where a function-local static paid its guard (a thread-local read) on every call.
const u32 g_index_promotion_budget = [] {
    const char* value = std::getenv("FAST_INDEX_BIND_PROMOTE");
    return value ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 8u;
}();
const u32 g_index_verify_period = [] {
    const char* value = std::getenv("FAST_INDEX_BIND_VERIFY");
    const int n = value ? std::atoi(value) : 0;
    return n > 0 ? static_cast<u32>(n) : 0u;
}();
const bool g_geometry_verify = std::getenv("FAST_GEOMETRY_VERIFY") != nullptr;
const bool g_tracked_small_buffers = std::getenv("VR_TRACKED_SMALL_BUFFERS") != nullptr;
const bool g_exact_upload_memo = std::getenv("VR_EXACT_UPLOAD_MEMO") != nullptr;
// VR_MOTION_READBACK_WAIT=1: ReadForMotion waits for its GPU copy, as before 2026-11-07.
const bool g_motion_readback_wait = [] {
    const char* value = std::getenv("VR_MOTION_READBACK_WAIT");
    return value && value[0] == '1';
}();
// FAST_UPLOAD_RETIRE (on unless '0', 2026-11-07): every upload hands the bytes it copied back to
// the CPU (SynchronizeBuffer), not ReadForMotion's alone.
const bool g_upload_retire = [] {
    const char* value = std::getenv("FAST_UPLOAD_RETIRE");
    return !value || value[0] != '0';
}();
const u32 g_small_read_direct_verify_period = [] {
    const char* v = std::getenv("FAST_SMALL_READ_DIRECT_VERIFY");
    const int n = v ? std::atoi(v) : 0;
    return n > 0 ? static_cast<u32>(n) : 0u;
}();
// FAST_CPU_VERTEX_MEMO (on unless '0'): BindVertexBuffers binds a range that ObtainBuffer would
// synchronize into the cache straight into its registered buffer when bit 34's hit test
// (ObtainIndexBuffer) finds the buffer there and the range CPU-clean (see BindVertexBuffers).
const bool g_vertex_memo = [] {
    const char* value = std::getenv("FAST_CPU_VERTEX_MEMO");
    return !value || value[0] != '0';
}();
// FAST_CPU_VERTEX_MEMO_VERIFY=1: every range the test would bind takes ObtainBuffer as before.
// That must return the same buffer and offset, and the locked tracker must find the range
// CPU-clean (LOG_ERROR on a mismatch; a write noticed meanwhile counts as a race).
const bool g_vertex_memo_verify = [] {
    const char* value = std::getenv("FAST_CPU_VERTEX_MEMO_VERIFY");
    return value && value[0] == '1';
}();
// FAST_CPU_VERTEX_MEMO's line (recording owner only, like BindVertexBuffers): merged ranges,
// ranges bound by the test, ranges left to ObtainBuffer's small-read path, misses.
struct VertexMemoStats {
    u64 ranges, hits, small, misses, verified, races, mismatches, streamed;
};
VertexMemoStats g_vertex_memo_stats{};

// FAST_CPU_VERTEX_STREAM (on unless '0', 2026-11-16): a direct non-indexed draw's merged vertex
// range larger than a caching page (one ObtainBuffer would synchronize into the cache) is copied
// into the stream buffer from its start to the end of the bytes the draw fetches, and bound there
// (as ObtainBuffer's small reads are), when those end within VertexStreamMaxBytes of its start and
// no page of them is GPU-modified. Central Yharnam's particles (the game's lingering fog among
// them) bind 4 MiB vertex buffers and fetch a few hundred bytes at their start while the CPU
// writes the next frame's on: each range ended on a freshly written page, whose upload ended the
// render pass (~19 times a frame, the particle pass drawn in ~20 pieces). Streamed: the same
// bytes, no upload, no pass break (GPU 6.72 -> 6.46 ms a frame there).
const bool g_vertex_stream = [] {
    const char* value = std::getenv("FAST_CPU_VERTEX_STREAM");
    return !value || value[0] != '0';
}();
constexpr u64 VertexStreamMaxBytes = 64_KB;
// The copy starts at the range's start aligned down to this, and lands at an offset so aligned:
// every element keeps its address's alignment modulo it, as in a cache buffer (page-aligned).
constexpr u64 VertexStreamAlign = 256;

// The end of the bytes a draw fetches from the vertex buffers starting in [base, base + size):
// each per-vertex buffer's elements [first_vertex, + vertex_count), each per-instance one's
// [first_instance, + instance_count) (a divisor fetches fewer of them), element e at the buffer's
// base + e * stride (stride 0: its base), at most 16 bytes (one attribute a buffer, at offset 0:
// GraphicsPipeline::GetVertexInputs). 0 when the stream must leave the range to the cache: a
// fetched element past its buffer's records (the binding's size bounds it there), a swizzled
// buffer, no fetched byte, or bytes past the range.
u64 VertexFetchEnd(const Vulkan::VertexView& inputs, const VertexFetch& fetch, VAddr base,
                   u64 size) {
    u64 end = 0;
    for (size_t k = 0; k < inputs.guest_buffers.size(); ++k) {
        const AmdGpu::Buffer& buffer = inputs.guest_buffers[k];
        const VAddr address = buffer.base_address;
        if (address == 0 || buffer.GetSize() == 0 || address < base || address - base >= size) {
            continue; // unbound, or in another merged range
        }
        if (k >= inputs.bindings.size() || buffer.swizzle_enable || buffer.add_tid_enable) {
            return 0;
        }
        const bool per_instance = inputs.bindings[k].inputRate == vk::VertexInputRate::eInstance;
        const u64 first = per_instance ? fetch.first_instance : fetch.first_vertex;
        const u64 count = per_instance ? fetch.instance_count : fetch.vertex_count;
        if (count == 0) {
            continue;
        }
        const u64 stride = buffer.GetStride();
        if (stride != 0 && first + count > buffer.num_records) {
            return 0;
        }
        const u64 last = stride == 0 ? 0 : (first + count - 1) * stride;
        end = std::max(end, address + last + 16);
    }
    return end > base && end - base <= size ? end : 0;
}

u32 IndexPromotionBudget() {
    return g_index_promotion_budget;
}

u32 IndexVerifyPeriod() {
    return g_index_verify_period;
}
} // namespace

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_pool{instance, scheduler},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      motion_staging{instance, scheduler, MemoryUsage::Download, MotionStagingSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BdaPageTableSize()},
      sparse_pool{instance} {
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");
    Vulkan::SetObjectName(instance.GetDevice(), bda_pagetable_buffer.Handle(),
                          "BDA Page Table Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);

    // Bit 33 (ObtainSmallDirect): read-back linear images reach guest memory through the backing
    // (TextureCache::DownloadImageMemory), unseen by the tracker, so a cached buffer could keep
    // older bytes than the stream copy would take.
    small_direct_supported = !EmulatorSettings.IsReadbackLinearImagesEnabled();
    if (!small_direct_supported) {
        LOG_INFO(Render_Vulkan, "FAST small_read_direct: unavailable with readback_linear_images");
    }

    // Before every submit of this scheduler: the motion snapshot copies asked for since the last
    // one (ReadForMotion), then the stream buffer's shadow (VR_STREAM_MEM, buffer.cpp) into the
    // GPU's memory.
    scheduler.SetPreSubmitHook([this, shadow = stream_buffer.HasShadow()] {
        FlushMotionCopies();
        if (shadow) {
            stream_buffer.FlushShadow();
        }
    });
    // Sparse views (FAST_BUFFER_ARENA=1): their binds go out right before each submit.
    if (sparse_pool.Enabled()) {
        // On their own queue when the device had one (GetSparseQueue), else the graphics one
        // the scheduler passes (under its lock).
        scheduler.SetSparseBinds(
            [this](vk::Queue) { return sparse_pool.SubmitBinds(instance.GetSparseQueue()); },
            sparse_pool.Timeline());
    }

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Set up garbage collection parameters
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    // FAST_BUFFER_EVICT_CARD_MB=N (a test): the marks as on a card with a budget of N MiB.
    const char* card_mb = std::getenv("FAST_BUFFER_EVICT_CARD_MB");
    const s64 device_local_memory =
        card_mb ? static_cast<s64>(std::strtoull(card_mb, nullptr, 10)) << 20
                : static_cast<s64>(instance.GetTotalMemoryBudget());
    SetMemoryMarks(device_local_memory);
    start_budget = card_mb ? 0 : device_local_memory; // a test's card is not revised
    LOG_INFO(Render_Vulkan, "Buffer cache: VRAM budget {} MiB{}, eviction under pressure from {} "
             "MiB, critical from {} MiB",
             device_local_memory >> 20, card_mb ? " (FAST_BUFFER_EVICT_CARD_MB)" : "",
             trigger_gc_memory >> 20, critical_gc_memory >> 20);
}

BufferCache::~BufferCache() {
    FastRender::WaitHostCopies();
    scheduler.SetPreSubmitHook(Common::UniqueFunction<void>{});
    if (sparse_pool.Enabled()) {
        // Every deferred release into the pool has run by now (~Rasterizer drains both queues
        // before any cache goes).
        scheduler.SetSparseBinds({}, {});
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool emulator_write,
                                   bool protection_only) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    if (emulator_write && MemoryTracker::KeepGpuPages()) {
        // The emulator writes exactly these bytes (a protection change none): the GPU's beside
        // them on their pages stay.
        memory_tracker->InvalidateRegion(
            device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); },
            false, true, protection_only ? 0 : size);
        return;
    }
    // An unmap (or a Linux uffd guest fault): no GPU byte of it is kept.
    // The catch-all log (off for performance):
    // if (MemoryTracker::KeepGpuPages() && gpu_modified_ranges.Intersects(device_addr, size)) {
    //     static std::atomic<u64> logs{0};
    //     const u64 n = logs.fetch_add(1, std::memory_order_relaxed);
    //     if (n < 64 || (n & 255) == 0) {
    //         LOG_INFO(Render_Vulkan,
    //                  "FAST_GPU_PAGE_KEEP: unmap of {:#x}..{:#x} drops GPU-written bytes ({} "
    //                  "so far)",
    //                  device_addr, device_addr + size, n + 1);
    //     }
    // }
    memory_tracker->ForgetKept(device_addr, size);
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::NoteFreshRange(VAddr addr, u64 size) {
    // Pages wholly inside: no GPU byte of theirs survives a CPU write from now on (a fault keeps
    // nothing aside). Pages shared with older data keep their GPU state; their dead byte ranges go
    // in ProcessFresh.
    memory_tracker->ForgetKept(addr, size);
    const VAddr pages_start = Common::AlignUp(addr, TRACKER_BYTES_PER_PAGE);
    const VAddr pages_end = Common::AlignDown(addr + size, TRACKER_BYTES_PER_PAGE);
    if (pages_start < pages_end) {
        memory_tracker->UnmarkRegionAsGpuModified(pages_start, pages_end - pages_start);
    }
    std::scoped_lock lk{fresh_mutex};
    fresh_ranges.emplace_back(addr, size);
    fresh_pending.store(true, std::memory_order_release);
}

void BufferCache::ProcessFresh() {
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{fresh_mutex};
        ranges.swap(fresh_ranges);
        fresh_pending.store(false, std::memory_order_release);
    }
    for (const auto& [addr, size] : ranges) {
        gpu_modified_ranges.Subtract(addr, size);
    }
}

void BufferCache::InvalidateWriteFault(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); },
        true);
}

void BufferCache::UnprotectCleanWindow(VAddr device_addr, u64 size) {
    memory_tracker->MarkCleanPagesCpuModified(device_addr, size);
}

void BufferCache::MarkFaultWindow(VAddr device_addr, u64 size, FaultWindowCounts* counts) {
    memory_tracker->MarkFaultWindow(device_addr, size, counts);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    Common::MemoryWriteTrace::Event("buffer.read.request", device_addr, size, is_write);
    liverpool->SendCommand<true>([this, device_addr, size, is_write] {
        MaterializeRetiles(device_addr, size, RetileDownload);
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
        // GPU-modified ranges come as many small scattered islands, so the download
        // is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr buf_start = buffer.CpuAddr();
        const VAddr buf_end = buf_start + buffer.SizeBytes();
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), buf_start);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), buf_end);
        DownloadBufferMemory<false>(buffer, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    });
}

std::span<const u8> BufferCache::ReadForMotion(VAddr address, u32 size, u64 frame) {
    FastRender::AssertRecordingAccess();
    if (!size || size > 16_MB) return {};
    MaterializeRetiles(address, size, RetileMotion);
    if (!HasGpuWrites(address, size)) {
        return {reinterpret_cast<const u8*>(address), size};
    }
    const BufferId buffer_id = FindBuffer(address, size);
    Buffer& buffer = slot_buffers[buffer_id];
    // This is the same upload the upcoming indexed draw needs. It retires only
    // CPU-owned bytes; it never downloads old GPU contents into guest memory.
    SynchronizeBuffer(buffer, address, size, false, false, true);
    if (!HasGpuWrites(address, size)) {
        Common::MemoryWriteTrace::Event("motion.index.cpu_reclaimed", address, size);
        return {reinterpret_cast<const u8*>(address), size};
    }
    const u64 serial = buffer.perf_serial;
    const u64 use = ++motion_snapshot_use;
    // A snapshot stays this request's until a write into its own bytes (NoteMotionWrite) or a
    // new buffer under them: writes elsewhere in a large buffer leave it alone (2026-11-08; the
    // buffer's write counter, as before, refreshed every character of a merged buffer each frame).
    if (auto found = MotionSnapshotBound(address, size);
        found != motion_snapshots.end() && found->address == address && found->size == size) {
        if (found->serial == serial && found->state != MotionState::Stale) {
            found->last_use = use;
            found->last_frame = frame;
            if (found->state == MotionState::Pending ||
                !found->data->ready.load(std::memory_order_acquire)) {
                ++motion_reads.waiting;
                return {};
            }
            ++motion_reads.snapshots;
            return found->data->bytes;
        }
        // A stale version of this exact request cannot be reused. Remove it before
        // allocating its replacement so repeated writes cannot consume all cache slots.
        motion_snapshot_bytes -= size;
        motion_snapshots.erase(found);
        ++motion_reads.replaced;
    }
    // Vulkan buffer copies are dword-aligned, including 16-bit indices starting at +2.
    const u64 begin = Common::AlignDown(static_cast<u64>(buffer.Offset(address)), u64{4});
    const u64 prefix = buffer.Offset(address) - begin;
    const u64 transfer_size = Common::AlignUp(prefix + size, u64{4});
    ASSERT(begin + transfer_size <= buffer.SizeBytes());
    // Where the copy goes, decided before the cache makes room for it. The old wait takes the
    // shared download ring; otherwise motion copies have a ring of their own whose bytes come back
    // only once a copy has landed (the GPU finishing is not enough: its deferred operation may run
    // later) or was dropped unrecorded, and nothing waits for room. No room, or a request larger
    // than the ring (none seen so far): camera motion for this draw, asked again next time.
    StreamBuffer& staging = g_motion_readback_wait ? download_buffer : motion_staging;
    u8* download{};
    u64 staging_offset{}, staging_bytes{};
    if (g_motion_readback_wait) {
        std::tie(download, staging_offset) = download_buffer.Map(transfer_size);
        download_buffer.Commit();
    } else {
        // The oldest copies that have landed (or were dropped) give their bytes back, in ring
        // order.
        while (!motion_staging_uses.empty() &&
               (motion_staging_uses.front().data->ready.load(std::memory_order_acquire) ||
                motion_staging_uses.front().data->dropped)) {
            motion_staging_used -= motion_staging_uses.front().bytes;
            motion_staging_uses.pop_front();
        }
        // An empty ring starts over: a copy that would not fit before its end must not wait
        // for another copy to move the position (reviewed 2026-11-07).
        if (motion_staging_uses.empty()) {
            motion_staging_head = 0;
        }
        const u64 at = motion_staging_head % MotionStagingSize;
        // A copy never wraps: the ring's tail is skipped and held with it.
        const u64 skip = at + transfer_size > MotionStagingSize ? MotionStagingSize - at : 0;
        staging_bytes = skip + transfer_size;
        if (transfer_size > MotionStagingSize ||
            motion_staging_used + staging_bytes > MotionStagingSize) {
            ++motion_reads.no_room;
            return {};
        }
        staging_offset = (at + skip) % MotionStagingSize;
        download = motion_staging.mapped_data.data() + staging_offset;
    }
    // Bound both allocations and entry count. Sized for a crowded scene's characters (100-170
    // motion draws a frame): with 16 entries / 16 MB the same snapshots could be evicted and
    // copied again every frame. Never one used in this frame: with more requests a frame than
    // room, evicting the least recently used made every request evict one needed again later in
    // the frame (nothing ever reused, a copy for each). This request keeps camera motion instead,
    // and the snapshots held keep their characters' object motion (a code review, 2026-11-08).
    // The motion ring's bytes are not taken yet (the wait's stream ring retires its own).
    while (!motion_snapshots.empty() &&
           (motion_snapshots.size() >= 256 || motion_snapshot_bytes + size > 64_MB)) {
        const auto oldest = std::min_element(motion_snapshots.begin(), motion_snapshots.end(),
            [](const auto& a, const auto& b) { return a.last_use < b.last_use; });
        if (oldest->last_frame == frame) {
            ++motion_reads.full;
            return {};
        }
        motion_snapshot_bytes -= oldest->size;
        motion_snapshots.erase(oldest);
    }
    if (motion_snapshots.empty()) {
        motion_snapshot_longest = 0;
    }
    auto data = std::make_shared<MotionBytes>();
    data->bytes.resize(size);
    if (!g_motion_readback_wait) {
        motion_staging_head += staging_bytes;
        motion_staging_used += staging_bytes;
        motion_staging_uses.push_back({staging_bytes, data});
    }
    motion_snapshots.insert(MotionSnapshotBound(address, size),
                            MotionSnapshot{address, size, MotionState::Pending, serial, use, frame,
                                           data});
    motion_snapshot_bytes += size;
    motion_snapshot_longest = std::max(motion_snapshot_longest, size);
    // Recorded with every other copy asked for before the next submit (FlushMotionCopies): one
    // barrier for them all instead of a pass break and a wait for all earlier GPU work per copy.
    motion_copies.push_back({
        .address = address,
        .size = size,
        .buffer_id = buffer_id,
        .serial = serial,
        .staging = staging.Handle(),
        .region = vk::BufferCopy{begin, staging_offset, transfer_size},
        .prefix = prefix,
        .host = download,
        .coherent = staging.is_coherent,
        .allocation = staging.buffer.allocation,
        .data = data,
    });
    ++motion_reads.copies;
    Common::MemoryWriteTrace::Event("motion.index.private_snapshot", address, size, transfer_size);
    // The copy lands a frame or two later; until then the draw keeps camera motion. Waiting
    // here stopped the draw recording until the GPU had run everything queued, once per copy:
    // the frame rate that sank after many areas (2026-11-07).
    if (!g_motion_readback_wait) {
        return {};
    }
    // VR_MOTION_READBACK_WAIT=1: recorded now and waited for, as before 2026-11-07.
    const auto land = RecordMotionCopies();
    scheduler.Finish();
    if (!land) {
        return {};
    }
    land();
    ++motion_reads.snapshots;
    return data->bytes;
}

std::vector<BufferCache::MotionSnapshot>::iterator BufferCache::MotionSnapshotBound(VAddr address,
                                                                                    u32 size) {
    return std::lower_bound(motion_snapshots.begin(), motion_snapshots.end(),
                            std::pair{address, size}, [](const MotionSnapshot& entry, const auto& key) {
                                return std::pair{entry.address, entry.size} < key;
                            });
}

void BufferCache::MarkMotionWrite(VAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    // Sorted by address, none longer than the longest held: the first that can hold a byte of the
    // write starts less than that far before it.
    const VAddr end = address + size;
    const VAddr from = address > motion_snapshot_longest ? address - motion_snapshot_longest : 0;
    for (auto it = MotionSnapshotBound(from, 0); it != motion_snapshots.end() && it->address < end;
         ++it) {
        if (it->address + it->size > address) {
            // Before its copy as well: a write noted ahead of the draw that does it (a written
            // binding, a submit in between) would land after a copy at this submit.
            it->state = MotionState::Stale;
        }
    }
}

Common::UniqueFunction<void> BufferCache::RecordMotionCopies() {
    if (motion_copies.empty()) {
        return {};
    }
    struct Landing {
        std::shared_ptr<MotionBytes> data;
        const u8* host;
        u64 prefix, offset, size;
        bool coherent;
        VmaAllocation allocation;
    };
    std::vector<Landing> lands;
    lands.reserve(motion_copies.size());
    std::optional<decltype(scheduler.Commands())> commands;
    for (const auto& copy : motion_copies) {
        // Still the request's snapshot, unwritten since it was asked for, and its buffer the one
        // it was asked from. A buffer deleted this tick (merged, collected) stays allocated until
        // the GPU is done with the tick, but its snapshots never match again: dropped, as are
        // replaced, evicted and stale ones (their staging bytes go back to the ring).
        const auto it = MotionSnapshotBound(copy.address, copy.size);
        const bool listed = it != motion_snapshots.end() && it->data == copy.data &&
                            it->state == MotionState::Pending;
        const bool source = slot_buffers.is_allocated(copy.buffer_id) &&
                            slot_buffers[copy.buffer_id].perf_serial == copy.serial &&
                            !slot_buffers[copy.buffer_id].is_deleted;
        if (!listed || !source) {
            copy.data->dropped = true;
            continue;
        }
        if (!commands) {
            scheduler.EndRendering();
            commands.emplace(scheduler.Commands());
            // Every write recorded before is done and visible to the copies. The reads before
            // need no order.
            const vk::MemoryBarrier2 written{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            };
            commands->pipelineBarrier2(
                vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        }
        commands->copyBuffer(slot_buffers[copy.buffer_id].Handle(), copy.staging, copy.region);
        it->state = MotionState::Copied;
        lands.push_back({copy.data, copy.host, copy.prefix, copy.region.dstOffset,
                         copy.region.size, copy.coherent, copy.allocation});
    }
    motion_copies.clear();
    if (lands.empty()) {
        return {};
    }
    // Completion alone does not replace the device-to-host visibility dependency.
    const vk::MemoryBarrier2 copied{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    commands->pipelineBarrier2(
        vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &copied});
    // By value: the deferred operation runs after the GPU is done with this tick, on any thread.
    // Each `ready` also gives that copy's staging bytes back.
    return [lands = std::move(lands), allocator = instance.GetAllocator()] {
        for (const auto& land : lands) {
            if (!land.coherent) {
                vmaInvalidateAllocation(allocator, land.allocation, land.offset, land.size);
            }
            std::memcpy(land.data->bytes.data(), land.host + land.prefix, land.data->bytes.size());
            land.data->ready.store(true, std::memory_order_release);
        }
    };
}

void BufferCache::FlushMotionCopies() {
    if (auto land = RecordMotionCopies()) {
        scheduler.DeferOperation(std::move(land));
    }
}

template <bool async>
void BufferCache::DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size) {
    Common::MemoryWriteTrace::Event("buffer.download.request", device_addr, size, async, scheduler.CurrentTick());
    ScratchProfile::Cpu scratch_cpu(3,"BufferCache::DownloadBufferMemory");
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(vk::BufferCopy{
                    .srcOffset = new_offset,
                    .dstOffset = total_size_bytes,
                    .size = new_size,
                });
                // Align up to avoid cache conflicts
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
            };
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
            gpu_modified_ranges.Subtract(device_addr_out, range_size);
            copy_memo_epoch.fetch_add(1, std::memory_order_acq_rel); // perf_copy_memo
        });
    if (total_size_bytes == 0) {
        return;
    }
    const auto [download, offset] = download_buffer.Map(total_size_bytes);
    for (auto& copy : copies) {
        // Modify copies to have the staging offset in mind
        copy.dstOffset += offset;
    }
    download_buffer.Commit();
    scheduler.EndRendering();
    ScratchProfile::Gpu scratch_gpu(scheduler,13);
    const auto cmdbuf = scheduler.Commands();
    // Synchronize prior GPU writes to this buffer before the transfer read
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer.buffer,
        .offset = 0,
        .size = buffer.SizeBytes(),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(buffer.buffer, download_buffer.Handle(), copies);
    // By value, and moved into the deferred operation: the async path runs it after this
    // function has returned. UniqueFunction made from a named (lvalue) lambda keeps a reference
    // to it, so before 2026-10-16 the deferred write read this dead stack frame (the crashes at
    // the first FAST_GPU_SETTLE: a garbage pointer, then a garbage guest address).
    auto write_data = [copies, buffer_addr = buffer.CpuAddr(), download, offset,
                             device_addr, size, tracker = memory_tracker.get()]() {
        auto* memory = Core::Memory::Instance();
        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer_addr + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - offset;
            Common::MemoryWriteTrace::Event("buffer.download.write", copy_device_addr, copy.size,
                Common::MemoryWriteTrace::SourceWord(download + dst_offset, copy.size));
            memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr), download + dst_offset,
                                    copy.size);
        }
        tracker->UnmarkRegionAsGpuModified(device_addr, size);
    };
    if constexpr (async) {
        scheduler.DeferOperation(std::move(write_data));
    } else {
        scheduler.Finish();
        write_data();
    }
}

void BufferCache::PrepareVertexInputs(const Vulkan::GraphicsPipeline& pipeline,
                                     u32 step_rate_0, u32 step_rate_1,
                                     Vulkan::VertexPreparation& output) {
    output.attributes.clear();
    output.bindings.clear();
    output.guest_buffers.clear();
    output.ranges.clear();
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    pipeline.GetVertexInputs(output.attributes, output.bindings, divisors, output.guest_buffers,
                             step_rate_0, step_rate_1);
    u64 hash = 14695981039346656037ull;
    for (const auto& buffer : output.guest_buffers) {
        if (pipeline.HasVrMotion())
            for (const auto byte : std::as_bytes(std::span(&buffer, 1)))
                hash = (hash ^ std::to_integer<u8>(byte)) * 1099511628211ull;
        if (buffer.base_address && buffer.GetSize())
            output.ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
    }
    output.geometry = !pipeline.HasVrMotion() || output.guest_buffers.empty() ? 0 : hash;
    std::ranges::sort(output.ranges, {}, &Vulkan::VertexPreparation::Range::base_address);
    size_t count = 0;
    for (const auto range : output.ranges) {
        if (!count || output.ranges[count - 1].end_address < range.base_address)
            output.ranges[count++] = range;
        else
            output.ranges[count - 1].end_address =
                std::max(output.ranges[count - 1].end_address, range.end_address);
    }
    output.ranges.resize(count);
}

void BufferCache::BindVertexBuffers(
    const Vulkan::GraphicsPipeline& pipeline,
    boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
    u64* motion_geometry, const Vulkan::VertexView* prepared, Vulkan::VertexBinds* binds,
    const VertexFetch* fetch) {
    ScratchProfile::Cpu scratch_vertex(30,"BufferCache::BindVertexBuffers");
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::VertexIndex};
    const auto& regs = ScratchDraw::Registers(liverpool);
    Vulkan::VertexPreparation local;
    const bool verify = g_geometry_verify;
    if (!prepared || verify) {
        PrepareVertexInputs(pipeline, regs.vgt_instance_step_rate_0,
                            regs.vgt_instance_step_rate_1, local);
        if (prepared) {
            ASSERT_MSG(std::ranges::equal(local.attributes, prepared->attributes) &&
                       std::ranges::equal(local.bindings, prepared->bindings) &&
                       local.guest_buffers.size() == prepared->guest_buffers.size() &&
                       std::memcmp(local.guest_buffers.data(), prepared->guest_buffers.data(),
                                   local.guest_buffers.size() * sizeof(AmdGpu::Buffer)) == 0 &&
                       std::ranges::equal(local.ranges, prepared->ranges) &&
                       local.geometry == prepared->geometry,
                       "Stage A vertex preparation differs from ordered replay");
        }
    }
    struct Coverage { u64 calls{}, prepared{}, recomputed{}, verified{}; };
    static thread_local Coverage coverage;
    ++coverage.calls;
    coverage.prepared += prepared != nullptr;
    coverage.recomputed += prepared == nullptr;
    coverage.verified += prepared && verify;
    if ((coverage.calls & 65535) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST geometry: calls={} prepared={} recomputed={} verified={}",
                 coverage.calls, coverage.prepared, coverage.recomputed, coverage.verified);
    const Vulkan::VertexView inputs = prepared ? *prepared : Vulkan::VertexView{local};
    const auto& guest_buffers = inputs.guest_buffers;
    if (motion_geometry) *motion_geometry = inputs.geometry;
    if (instance.IsVertexInputDynamicState()) {
        if (binds) {
            binds->vertex_input = true;
            binds->bindings.assign(inputs.bindings.begin(), inputs.bindings.end());
            binds->attributes.assign(inputs.attributes.begin(), inputs.attributes.end());
        } else {
            scheduler.Commands().setVertexInputEXT(inputs.bindings, inputs.attributes);
        }
    }
    if (inputs.bindings.empty()) return;
    struct BufferRange {
        VAddr base_address, end_address;
        vk::Buffer vk_buffer;
        u64 offset;
        VAddr stream_end; // FAST_CPU_VERTEX_STREAM: its stream copy ends here (0: not streamed)
        size_t GetSize() const { return end_address - base_address; }
    };
    Vulkan::VertexInputs<BufferRange> ranges_merged;
    for (const auto& range : inputs.ranges)
        ranges_merged.push_back({range.base_address, range.end_address, {}, 0, 0});

    // FAST_CPU_VERTEX_MEMO: a range ObtainBuffer synchronizes into the cache (over 16 KB, or any
    // with VR_TRACKED_SMALL_BUFFERS; smaller ones keep its stream copy or bit 33) is bound in the
    // buffer the page table registers over its start when that buffer holds all of it and every
    // page is CPU-clean by bit 17's unlocked test: bit 34's hit test. FindBuffer returns that
    // buffer (it is in bounds), and SynchronizeBuffer uploads nothing: with bit 24 on and copy
    // threads off its own test per region is this same unlocked one (MemoryTracker::
    // ForEachUploadRange), so the decision is the old path's, without its calls, scopes and
    // upload vector. The GPU-modified query and its barrier below are the same.
    const bool vertex_memo = g_vertex_memo && FastRender::On(UploadEarlyExit) &&
                             !FastRender::On(FastRender::CopyThreads);
    auto& memo_stats = g_vertex_memo_stats;
    const auto clean_buffer = [this](VAddr address, u32 size) -> BufferId {
        if (address + size > (u64{1} << MemoryTracker::MAX_CPU_PAGE_BITS)) {
            return {};
        }
        const PageData* page = page_table.find(address >> CACHING_PAGEBITS);
        if (page == nullptr || !page->buffer_id) {
            return {};
        }
        const Buffer& buffer = slot_buffers[page->buffer_id];
        return !buffer.is_deleted && buffer.IsInBounds(address, size) &&
                       memory_tracker->IsRegionCpuCleanRelaxed(address, size)
                   ? page->buffer_id
                   : BufferId{};
    };
    // FAST_CPU_VERTEX_STREAM: where the stream copy of a range would end (0: the cache's way).
    const auto stream_end_of = [&](const BufferRange& range, u64 size) -> VAddr {
        const VAddr end = VertexFetchEnd(inputs, *fetch, range.base_address, size);
        if (end == 0) {
            return 0;
        }
        const VAddr copy_start = Common::AlignDown(range.base_address, VertexStreamAlign);
        const u64 bytes = end - copy_start;
        if (bytes > VertexStreamMaxBytes) {
            return 0;
        }
        // Every buffer of the range is bound within the copy (pack cuts its size at the end).
        for (const auto& b : guest_buffers) {
            if (b.base_address != 0 && b.GetSize() != 0 && b.base_address >= range.base_address &&
                b.base_address < range.end_address && b.base_address >= end) {
                return 0;
            }
        }
        // Only a range the cache already holds in one buffer and the CPU has written since (the
        // particles', rewritten every frame): one the cache has never held goes its way, uploaded
        // once and then bound from that buffer (the memo), not copied again at every draw (the
        // 2026-11-16 review). The cache holds only mapped memory, and the bytes before the range's
        // start lie in its first page. None of it written by the GPU: the guest bytes are the
        // range's contents, as for a small read.
        if (range.base_address + size > (u64{1} << MemoryTracker::MAX_CPU_PAGE_BITS)) {
            return 0;
        }
        const PageData* page = page_table.find(range.base_address >> CACHING_PAGEBITS);
        if (page == nullptr || !page->buffer_id) {
            return 0;
        }
        const Buffer& held = slot_buffers[page->buffer_id];
        if (held.is_deleted || !held.IsInBounds(range.base_address, size) ||
            // With the memo off, a miss tells us nothing about CPU writes. Let the usual
            // synchronized bind handle a clean range instead of streaming it every draw.
            (!vertex_memo && memory_tracker->IsRegionCpuCleanRelaxed(range.base_address, size)) ||
            IsRegionGpuModified(copy_start, bytes)) {
            return 0;
        }
        return end;
    };

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        MaterializeRetiles(range.base_address, size, RetileVertex);
        const bool memo_range =
            vertex_memo && (size > CACHING_PAGESIZE || g_tracked_small_buffers);
        const BufferId clean_id =
            memo_range ? clean_buffer(range.base_address, static_cast<u32>(size)) : BufferId{};
        if (clean_id) {
            NoteUsed(clean_id);
        }
        Buffer* const clean = clean_id ? &slot_buffers[clean_id] : nullptr;
        if constexpr (Common::Log::Enabled) {
            if (vertex_memo) {
                ++memo_stats.ranges;
                memo_stats.small += !memo_range;
                memo_stats.misses += memo_range && !clean;
                if ((memo_stats.ranges & 0xFFFF) == 0) {
                    LOG_INFO(Render_Vulkan,
                             "FAST_CPU_VERTEX_MEMO: ranges={} hits={} small={} misses={} "
                             "verified={} races={} mismatches={} streamed={}",
                             memo_stats.ranges, memo_stats.hits, memo_stats.small,
                             memo_stats.misses, memo_stats.verified, memo_stats.races,
                             memo_stats.mismatches, memo_stats.streamed);
                }
            }
        }
        Buffer* buffer = clean;
        bool queried = false; // a cache buffer: the GPU-modified query below is the old path's
        // FAST_CPU_VERTEX_STREAM: a range the cache would synchronize, its fetched bytes copied.
        const VAddr stream_end = !clean && fetch && g_vertex_stream && size > CACHING_PAGESIZE
                                     ? stream_end_of(range, size)
                                     : 0;
        if (clean && !g_vertex_memo_verify) {
            if constexpr (Common::Log::Enabled) {
                ++memo_stats.hits;
            }
            range.vk_buffer = clean->buffer;
            range.offset = static_cast<u32>(clean->Offset(range.base_address));
        } else if (stream_end != 0) {
            const VAddr copy_start = Common::AlignDown(range.base_address, VertexStreamAlign);
            const u64 at =
                stream_buffer.Copy(copy_start, stream_end - copy_start, VertexStreamAlign);
            buffer = &stream_buffer;
            range.vk_buffer = stream_buffer.buffer;
            range.offset = at + (range.base_address - copy_start);
            range.stream_end = stream_end;
            queried = true; // as for a small read: no page of the copy is GPU-modified
            if constexpr (Common::Log::Enabled) {
                ++memo_stats.streamed;
            }
        } else {
            // FAST_CPU_VERTEX_MEMO_VERIFY: what the test found (read before ObtainBuffer, which
            // could create or join buffers if it disagreed), the range's tracking epoch and the
            // locked tracker's answer (ObtainBuffer uploads what that finds CPU-modified).
            const VAddr clean_address = clean ? clean->CpuAddr() : 0;
            const u32 clean_offset =
                clean ? static_cast<u32>(clean->Offset(range.base_address)) : 0;
            const u64 epoch = clean ? TrackingEpoch(range.base_address, size) : 0;
            const bool cpu_modified =
                clean && memory_tracker->IsRegionCpuModified(range.base_address, size);
            SmallRead small_read{};
            const auto [obtained, offset] =
                ObtainBuffer(range.base_address, size, false, false, {}, &small_read);
            buffer = obtained;
            range.vk_buffer = obtained->buffer;
            range.offset = offset;
            // Bit 24: the stream buffer (and bit 33's in-place binding) is returned only after
            // the same GPU-modified query.
            queried = obtained == &stream_buffer || small_read.direct;
            if (clean) {
                const bool same = obtained == &slot_buffers[clean_id] && offset == clean_offset;
                if constexpr (Common::Log::Enabled) {
                    ++memo_stats.verified;
                    if (!same || cpu_modified) {
                        if (TrackingEpoch(range.base_address, size) != epoch) {
                            ++memo_stats.races; // a write noticed meanwhile: the test raced it
                        } else if (++memo_stats.mismatches <= 64) {
                            LOG_ERROR(Render_Vulkan,
                                      "FAST_CPU_VERTEX_MEMO verify: range {:#x} size {} clean in "
                                      "buffer {:#x}, ObtainBuffer gave buffer {:#x} offset {} "
                                      "(same={}), locked CPU-modified={}, mismatch #{}",
                                      range.base_address, size, clean_address,
                                      obtained->CpuAddr(), offset, same, cpu_modified,
                                      memo_stats.mismatches);
                        }
                    }
                }
            }
        }
        // (A streamed range's copy holds no GPU-written byte: no barrier, with bit 24 off too.)
        if (stream_end == 0 && !(queried && FastRender::On(UploadEarlyExit)) &&
            IsRegionGpuModified(range.base_address, size)) {
            if (auto barrier =
                    buffer->GetBarrier(vk::AccessFlagBits2::eVertexAttributeRead,
                                       vk::PipelineStageFlagBits2::eVertexAttributeInput)) {
                barriers.emplace_back(*barrier);
            }
        }
    }

    const auto pack = [&](auto& host_buffers, auto& host_offsets,
                          auto& host_sizes, auto& host_strides) {
        for (const auto& buffer : guest_buffers) {
            u64 size = buffer.GetSize();
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                const auto host_buffer_info =
                    std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                        return buffer.base_address >= range.base_address &&
                               buffer.base_address < range.end_address;
                    });
                ASSERT(host_buffer_info != ranges_merged.cend());
                host_buffers.emplace_back(host_buffer_info->vk_buffer);
                host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                       host_buffer_info->base_address);
                // FAST_CPU_VERTEX_STREAM: a streamed range's buffers end where its copy ends
                // (every one starts before it: stream_end_of).
                if (host_buffer_info->stream_end != 0) {
                    size = std::min<u64>(size, host_buffer_info->stream_end - buffer.base_address);
                }
            } else {
                host_buffers.emplace_back(VK_NULL_HANDLE);
                host_offsets.push_back(0);
            }
            host_sizes.push_back(size);
            host_strides.push_back(buffer.GetStride());
        }
    };
    if (binds) {
        binds->buffers.clear();
        binds->offsets.clear();
        binds->sizes.clear();
        binds->strides.clear();
        pack(binds->buffers, binds->offsets, binds->sizes, binds->strides);
        binds->two = !instance.IsVertexInputDynamicState();
        return;
    }
    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    pack(host_buffers, host_offsets, host_sizes, host_strides);
    const auto cmdbuf = scheduler.Commands();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void BufferCache::EncodeVertexBinds(const Vulkan::VertexBinds& binds,
                                    const Vulkan::RecorderProxy& cmdbuf) {
    if (binds.vertex_input) {
        cmdbuf.setVertexInputEXT(binds.bindings, binds.attributes);
    }
    if (!binds.buffers.empty()) {
        const u32 count = static_cast<u32>(binds.buffers.size());
        if (binds.two) {
            cmdbuf.bindVertexBuffers2(0, count, binds.buffers.data(), binds.offsets.data(),
                                      binds.sizes.data(), binds.strides.data());
        } else {
            cmdbuf.bindVertexBuffers(0, count, binds.buffers.data(), binds.offsets.data());
        }
    }
    if (binds.index) {
        cmdbuf.bindIndexBuffer(binds.index_buffer, binds.index_offset, binds.index_type);
    }
}

void BufferCache::BindIndexBuffer(
    u32 index_offset, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers,
    Vulkan::VertexBinds* binds) {
    // FAST_CPU_STAGEB_SPLIT: the bind as values for EncodeVertexBinds, else encoded here.
    const auto bind = [&](vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) {
        if (binds) {
            binds->index = true;
            binds->index_buffer = buffer;
            binds->index_offset = offset;
            binds->index_type = type;
        } else {
            scheduler.Commands().bindIndexBuffer(buffer, offset, type);
        }
    };
    ScratchProfile::Cpu scratch_index(31,"BufferCache::BindIndexBuffer");
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::VertexIndex};
    const auto& regs = ScratchDraw::Registers(liverpool);

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    // Bit 34: the bind itself is always recorded (other code binds index buffers into the same
    // command stream, e.g. VrInFrame::Record); only obtaining the buffer is shortened.
    if (FastRender::On(IndexBind)) {
        if (const auto bound = ObtainIndexBuffer(index_address, index_buffer_size, barriers)) {
            bind(bound->first->Handle(), bound->second, index_type);
            return;
        }
    }
    SmallRead small_read{};
    const auto [vk_buffer, offset] =
        ObtainBuffer(index_address, index_buffer_size, false, false, {}, &small_read);
    const bool queried = vk_buffer == &stream_buffer || small_read.direct;
    if (!(queried && FastRender::On(UploadEarlyExit)) &&
        IsRegionGpuModified(index_address, index_buffer_size)) {
        if (auto barrier = vk_buffer->GetBarrier(vk::AccessFlagBits2::eIndexRead,
                                                 vk::PipelineStageFlagBits2::eIndexInput)) {
            barriers.emplace_back(*barrier);
        }
    }
    bind(vk_buffer->Handle(), offset, index_type);
}

std::optional<std::pair<Buffer*, u32>> BufferCache::ObtainIndexBuffer(
    VAddr address, u32 size,
    boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers) {
    FastRender::AssertRecordingAccess();
    MaterializeRetiles(address, size, RetileIndex);
    if (!index_bind_memo) {
        index_bind_memo = std::make_unique<IndexBindMemo>(); // value-initialized: all zero
        index_bind_memo->tick = ~u64{0};
    }
    IndexBindMemo& memo = *index_bind_memo;
    if ((++memo.calls & 65535) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST index_bind: hits={} misses={} promotions={} volatile={} verified={} "
                 "races={} mismatches={}",
                 memo.hits, memo.misses, memo.promotions, memo.demotions, memo.verified,
                 memo.races, memo.mismatches);
    }
    // The barrier BindIndexBuffer adds for a cache buffer.
    const auto bind = [&](Buffer& buffer, bool gpu_modified) {
        if (gpu_modified) {
            if (auto barrier = buffer.GetBarrier(vk::AccessFlagBits2::eIndexRead,
                                                 vk::PipelineStageFlagBits2::eIndexInput)) {
                barriers.emplace_back(*barrier);
            }
        }
        return std::pair<Buffer*, u32>{&buffer, static_cast<u32>(buffer.Offset(address))};
    };
    // Empty, null or outside the tracked 40-bit space: the old path as it was.
    if (address == 0 || size == 0 ||
        address + size > (u64{1} << MemoryTracker::MAX_CPU_PAGE_BITS)) {
        ++memo.misses;
        return std::nullopt;
    }
    const u32 verify_period = IndexVerifyPeriod();
    const bool sample = verify_period != 0 && ++memo.verify_count % verify_period == 0;
    const u64 epoch = sample ? TrackingEpoch(address, size) : 0;

    // Hit: one registered buffer holds the whole range and no page of it was written by the CPU
    // since it was uploaded (bit 17's unlocked test, as bit 24's upload early exit uses). For a
    // large range ObtainBuffer would return this buffer and upload nothing; for a small one it
    // would copy the same bytes into the stream buffer. No CreateBuffer, no page-table allocation.
    if (const PageData* page = page_table.find(address >> CACHING_PAGEBITS);
        page != nullptr && page->buffer_id) {
        const BufferId buffer_id = page->buffer_id;
        Buffer& buffer = slot_buffers[buffer_id];
        if (!buffer.is_deleted && buffer.IsInBounds(address, size) &&
            memory_tracker->IsRegionCpuCleanRelaxed(address, size)) {
            const bool gpu_modified = IsRegionGpuModified(address, size);
            if (sample && !VerifyIndexHit(memo, buffer_id, address, size, epoch, gpu_modified)) {
                ++memo.misses;
                return std::nullopt;
            }
            ++memo.hits;
            NoteUsed(buffer_id);
            return bind(buffer, gpu_modified);
        }
    }
    ++memo.misses;
    if (size > CACHING_PAGESIZE) {
        return std::nullopt; // ObtainBuffer synchronizes it into the cache anyway
    }
    IndexBindMemo::Entry& entry = memo.entries[IndexBindMemo::Index(address, size)];
    if (entry.address != address || entry.size != size) {
        entry = IndexBindMemo::Entry{.address = address, .size = size,
                                     .state = IndexBindMemo::Seen};
        return std::nullopt;
    }
    if (entry.state == IndexBindMemo::Promoted) {
        // Written by the CPU (or its pages unprotected around another write) since the upload.
        // The copy, as before, costs less than an upload and a render pass break per rewrite.
        entry.state = IndexBindMemo::Volatile;
        ++memo.demotions;
        return std::nullopt;
    }
    if (entry.state != IndexBindMemo::Seen) {
        return std::nullopt;
    }
    // Bound before: promote it. Each promotion may upload, join buffers and break the render
    // pass once, so a burst of new geometry is spread over command buffers.
    const u64 tick = scheduler.CurrentTick();
    if (memo.tick != tick) {
        memo.tick = tick;
        memo.budget = IndexPromotionBudget();
    }
    if (memo.budget == 0 || !memory->IsValidMapping(address)) {
        return std::nullopt;
    }
    --memo.budget;
    entry.state = IndexBindMemo::Promoted;
    ++memo.promotions;
    // ObtainBuffer's cache path (read-only, not a texel buffer).
    const BufferId buffer_id = FindBuffer(address, size);
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, address, size, false, false);
    return bind(buffer, IsRegionGpuModified(address, size));
}

bool BufferCache::VerifyIndexHit(IndexBindMemo& memo, BufferId buffer_id, VAddr address,
                                 u32 size, u64 epoch, bool gpu_modified) {
    ++memo.verified;
    // The old path's own answers, under the locks: CPU-clean, and FindBuffer finds this buffer
    // (in bounds, so it creates nothing).
    const bool cpu_modified = memory_tracker->IsRegionCpuModified(address, size);
    const bool same_buffer = FindBuffer(address, size) == buffer_id;
    if (cpu_modified || !same_buffer) {
        if (TrackingEpoch(address, size) != epoch) {
            ++memo.races; // a write noticed meanwhile: the hit raced it, as a copy would
        } else {
            ++memo.mismatches;
            LOG_ERROR(Render_Vulkan,
                      "FAST index_bind verify: hit at {:#x} size {} but cpu_modified={} "
                      "same_buffer={}; this bind takes the copy path",
                      address, size, cpu_modified, same_buffer);
        }
        return false;
    }
    // GPU-written data is newer in the buffer than in guest memory (the old path binds the buffer
    // too); an untracked epoch cannot date a hash.
    if (gpu_modified || epoch == 0 || size > IndexBindMemo::MaxVerifyBytes ||
        !memory->IsValidMapping(address)) {
        return true;
    }
    // While the range's tracking epoch is unchanged no write was noticed and nothing uploaded, so
    // its guest bytes must equal those hashed at that epoch (what the buffer holds).
    thread_local std::vector<u8> bytes;
    bytes.resize(size);
    memory->CopySparseMemory(address, bytes.data(), size);
    const u64 hash = XXH3_64bits(bytes.data(), size);
    if (TrackingEpoch(address, size) != epoch) {
        ++memo.races;
        return true;
    }
    IndexBindMemo::Entry& entry = memo.entries[IndexBindMemo::Index(address, size)];
    if (entry.address == address && entry.size == size && entry.verify_epoch == epoch) {
        if (entry.verify_hash == hash) {
            return true;
        }
        ++memo.mismatches;
        LOG_ERROR(Render_Vulkan,
                  "FAST index_bind verify: bytes at {:#x} size {} changed with no write noticed "
                  "(tracking epoch {}); this bind takes the copy path",
                  address, size, epoch);
        entry.state = IndexBindMemo::Volatile;
        entry.verify_epoch = 0;
        return false;
    }
    if (entry.address != address || entry.size != size) {
        entry = IndexBindMemo::Entry{.address = address, .size = size,
                                     .state = IndexBindMemo::Seen};
    }
    entry.verify_epoch = epoch;
    entry.verify_hash = hash;
    return true;
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    Common::MemoryWriteTrace::Event("dma.fill", address, num_bytes, value, is_gds);
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        MaterializeRetiles(address, num_bytes, RetileFill);
        texture_cache.ClearMeta(address);
        if (!IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + num_bytes / sizeof(u32), value);
            return;
        }
    }
    Buffer* buffer = [&] {
        if (is_gds) {
            return &gds_buffer;
        }
        const auto [buffer, offset] = ObtainBuffer(address, num_bytes, true);
        return buffer;
    }();
    buffer->Fill(buffer->Offset(address), num_bytes, value);
}

void BufferCache::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    Common::MemoryWriteTrace::Event("dma.copy", dst, num_bytes, src);
    ScratchProfile::Cpu scratch_cpu(0,"BufferCache::CopyBuffer");
    if (!dst_gds && !IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
        // Without a readback there's nothing we can do with this
        // Fallback to creating dst buffer on GPU to at least have this data there
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    if (!src_gds) {
        MaterializeRetiles(src, num_bytes, RetileCopy);
    }
    if (!dst_gds) {
        MaterializeRetiles(dst, num_bytes, RetileCopy);
    }
    auto& src_buffer = [&] -> const Buffer& {
        if (src_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(src, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, src, num_bytes, false, true);
        return buffer;
    }();
    auto& dst_buffer = [&] -> const Buffer& {
        if (dst_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(dst, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, dst, num_bytes, true, true);
        gpu_modified_ranges.AddIfNew(dst, num_bytes);
        NoteGpuWrite(dst, num_bytes);
        return buffer;
    }();
    const vk::BufferCopy region = {
        .srcOffset = src_buffer.Offset(src),
        .dstOffset = dst_buffer.Offset(dst),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 buf_barriers_before[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    scheduler.EndRendering();
    ScratchProfile::Gpu scratch_gpu(scheduler,10);
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_before,
    });
    NativeStereo::Copy(scheduler, src_buffer.Handle(), dst_buffer.Handle(), region);
    const vk::BufferMemoryBarrier2 buf_barriers_after[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_after,
    });
}

namespace {
// VR_RETILE_DEFER (BufferCache::DeferImageRetile), read once: on unless '0'.
char RetileDeferMode() {
    static const char mode = [] {
        const char* value = std::getenv("VR_RETILE_DEFER");
        return value && (value[0] == '0' || value[0] == 'm') ? value[0] : '1';
    }();
    return mode;
}
constexpr std::array<const char*, BufferCache::NumRetileTriggers> RetileTriggerNames{
    "obtain", "copy", "fill", "download", "image upload", "index", "motion", "range sync",
    "dma", "vertex memo", "hle copy", "source gone", "overlap", "forced"};
} // namespace

bool BufferCache::DeferImageRetile(VAddr dst, u64 bytes, ImageId source,
                                   const vk::BufferImageCopy& region) {
    FastRender::AssertRecordingAccess();
    const char mode = RetileDeferMode();
    if (mode == '0' || retile_off || bytes == 0 ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled ||
        texture_cache.ReadsBackLinearImages() || FastRender::On(FastRender::CopyThreads)) {
        return false;
    }
    const VAddr hi = dst + bytes;
    for (u32 i = 0; i < retile_read_count; ++i) {
        if (retile_read[i].first < hi && dst < retile_read[i].second) {
            return false; // read before: recorded now, as always
        }
    }
    // This copy rewrites every byte of [dst, dst + bytes): a pending re-tile of exactly that
    // range is dropped unread; one overlapping it otherwise is recorded first.
    for (auto& retile : retiles) {
        if (!retile.valid || retile.hi <= dst || hi <= retile.lo) {
            continue;
        }
        if (retile.lo == dst && retile.hi == hi) {
            retile.valid = false;
            --retile_count;
            ++retile_superseded;
        } else {
            MaterializeRetile(retile, RetileOverlap);
        }
    }
    DeferredRetile* slot = nullptr;
    for (auto& retile : retiles) {
        if (!retile.valid) {
            slot = &retile;
            break;
        }
    }
    if (!slot) {
        return false; // all four in use: this one is recorded now
    }
    // The buffer state the recorded copy leaves (its CPU-modified pages uploaded first, then the
    // range GPU-modified), without the re-tile.
    [[maybe_unused]] const auto obtained = ObtainBuffer(dst, static_cast<u32>(bytes), true, true);
    const Image& image = texture_cache.GetImage(source);
    *slot = DeferredRetile{.lo = dst,
                           .hi = hi,
                           .image = source,
                           .image_uid = image.image_uid,
                           .write_serial = image.write_serial,
                           .region = region,
                           .valid = true};
    ++retile_count;
    if ((++retile_deferred % 1800) == 0) {
        ReportRetiles();
    }
    return true;
}

void BufferCache::MaterializeRetilesIn(VAddr addr, u64 size, u32 trigger) {
    const VAddr end = size > ~u64{0} - addr ? ~u64{0} : addr + size;
    for (auto& retile : retiles) {
        if (retile.valid && retile.lo < end && addr < retile.hi) {
            MaterializeRetile(retile, trigger);
        }
    }
}

void BufferCache::MaterializeRetilesFrom(ImageId image) {
    if (retile_count == 0) [[likely]] {
        return;
    }
    for (auto& retile : retiles) {
        if (retile.valid && retile.image == image) {
            MaterializeRetile(retile, RetileSourceGone);
        }
    }
}

bool BufferCache::IsRetileSource(ImageId image) const noexcept {
    if (retile_count == 0) [[likely]] {
        return false;
    }
    for (const auto& retile : retiles) {
        if (retile.valid && retile.image == image) {
            return true;
        }
    }
    return false;
}

void BufferCache::RetileFrameEnd() {
    if (RetileDeferMode() == 'm') {
        MaterializeAllRetiles(RetileForced);
    }
}

void BufferCache::MaterializeRetile(DeferredRetile& retile, u32 trigger) {
    FastRender::AssertRecordingAccess();
    // Taken off the list first: the lookup below may come back through a hooked entry point.
    retile.valid = false;
    --retile_count;
    ++retile_materialized;
    ++retile_triggers[trigger];
    Image& image = texture_cache.GetImage(retile.image);
    const u64 bytes = retile.hi - retile.lo;
    if (image.image_uid != retile.image_uid) {
        // Not expected: an unregistered source records its re-tiles first.
        LOG_ERROR(Render_Vulkan,
                  "VR retile defer: the source of {:#x}+{:#x} changed identity before its re-tile; "
                  "deferring off",
                  retile.lo, bytes);
        retile_off = true;
        return;
    }
    // No ObtainBuffer: the copy's upload and GPU-modified marking were done when it was
    // deferred, and CPU writes since then must stay pending to land over these bytes, as they
    // would have over the recorded copy.
    const bool late = image.write_serial != retile.write_serial;
    retile_late += late ? 1 : 0;
    Buffer& buffer = slot_buffers[FindBuffer(retile.lo, static_cast<u32>(bytes))];
    NativeStereo::Barrier(scheduler);
    texture_cache.GetTileManager().TileImage(image, std::span{&retile.region, 1}, buffer.Handle(),
                                             static_cast<u32>(buffer.Offset(retile.lo)),
                                             static_cast<u32>(bytes));
    NativeStereo::Barrier(scheduler);
    if (trigger != RetileForced && trigger != RetileOverlap && trigger != RetileSourceGone) {
        // A reader: from now on copies into this destination are recorded when made, as before.
        if (retile_read_count < retile_read.size()) {
            retile_read[retile_read_count++] = {retile.lo, retile.hi};
        } else {
            retile_off = true;
        }
        LOG_WARNING(Render_Vulkan,
                    "VR retile defer: {} read {:#x}+{:#x} (a re-tile of image {:#x}, {}); {}",
                    RetileTriggerNames[trigger], retile.lo, bytes, image.info.guest_address,
                    late ? "LATE: the image was written since the copy"
                         : "exact: the image is unchanged since the copy",
                    retile_off ? "deferring off for this session"
                               : "this destination is no longer deferred");
        ReportRetiles();
    }
}

void BufferCache::ReportRetiles() {
    std::string triggers;
    for (u32 i = 0; i < NumRetileTriggers; ++i) {
        if (retile_triggers[i] != 0) {
            triggers += fmt::format(" {} {}", RetileTriggerNames[i], retile_triggers[i]);
        }
    }
    LOG_INFO(Render_Vulkan,
             "VR retile defer: {} deferred, {} dropped unread, {} recorded later{}{} ({} after "
             "the source changed); pending {}",
             retile_deferred, retile_superseded, retile_materialized,
             triggers.empty() ? "" : " by", triggers, retile_late, retile_count);
}

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                  bool is_texel_buffer, BufferId buffer_id,
                                                  SmallRead* small_read) {
    FastRender::AssertRecordingAccess();
    ScratchProfile::Cpu scratch_obtain(17,"BufferCache::ObtainBuffer");
    MaterializeRetiles(device_addr, size, RetileObtain);
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    const bool tracked_small = g_tracked_small_buffers;
    if (!tracked_small && !is_written && size <= CACHING_PAGESIZE && !IsRegionGpuModified(device_addr, size)) {
        // Bit 33: already in a cached buffer and unchanged since its upload: bound there.
        if (small_read && small_read_direct) {
            if (const auto direct =
                    ObtainSmallDirect(device_addr, size, buffer_id, small_read->shader_read)) {
                small_read->direct = true;
                return *direct;
            }
        }
        ScratchProfile::Cpu scratch_stream(28,"ObtainBuffer::SmallReadStream");
        const bool memo_upload = g_exact_upload_memo;
        const u64 offset = memo_upload
            ? stream_buffer.CopyReadOnlyMemo(device_addr, size, instance.UniformMinAlignment())
            : stream_buffer.Copy(device_addr, size, instance.UniformMinAlignment());
        return {&stream_buffer, offset};
    }
    if (IsBufferInvalid(buffer_id)) {
        buffer_id = FindBuffer(device_addr, size);
    }
    Buffer& buffer = slot_buffers[NoteUsed(buffer_id)];
    // The normal read-only path leaves a tracked, CPU-clean range untouched. Drain fresh
    // ranges before testing it, and preserve the image-alias check for formatted reads.
    if (small_read && small_read->shader_read && !is_written && size != 0 &&
        FastRender::On(UploadEarlyExit) && !FastRender::On(FastRender::CopyThreads)) {
        TakeFresh();
        if (memory_tracker->IsRegionCpuCleanRelaxed(device_addr, size)) {
            if (is_texel_buffer) {
                SynchronizeBufferFromImage(buffer, device_addr, size);
            }
            return {&buffer, buffer.Offset(device_addr)};
        }
    }
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.AddIfNew(device_addr, size);
        NoteGpuWrite(device_addr, size);
    }
    return {&buffer, buffer.Offset(device_addr)};
}

namespace {
// FAST_SMALL_READ_DIRECT_VERIFY: read-back comparisons that found other bytes (any thread).
std::atomic<u64> g_small_direct_byte_mismatches{0};
// The same for sampled bit 44 bindings (also counted above).
std::atomic<u64> g_small_settle_byte_mismatches{0};

/// FastRender bit 44 (ObtainSmallDirect): where a small read's buffer stands when bit 33's own
/// state test failed. Created and TransferRead are bound in place.
enum class Settle : u8 {
    Created,      // shader read; memory read and write at every stage (new buffer, JoinOverlap)
    TransferRead, // shader read; a texture upload's read, every write visible to shader reads
    Write,        // the access last asked for may write (a shader write, JoinOverlap's copy)
    Narrow,       // shader read; a vertex, index or indirect read, writes visible to shader reads
    Unseen,       // shader read; a write not yet visible to shader reads
    Fixed,        // a vertex or index read: bit 33's own test only
    Count,
};

Settle ClassifySettle(const Buffer& buffer, bool shader_read) {
    if (!shader_read) {
        return Settle::Fixed;
    }
    constexpr vk::AccessFlags2 created =
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    constexpr vk::AccessFlags2 initial =
        created | vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
    const vk::AccessFlags2 access = buffer.access_mask;
    if (buffer.stage == vk::PipelineStageFlagBits2::eAllCommands &&
        (access == created || access == initial)) {
        return buffer.shader_reads_visible ? Settle::Created : Settle::Unseen;
    }
    if ((access & BarrierReadAccess) != access) {
        return Settle::Write;
    }
    if (!buffer.shader_reads_visible) {
        return Settle::Unseen;
    }
    if (buffer.stage == vk::PipelineStageFlagBits2::eTransfer &&
        access == vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead}) {
        return Settle::TransferRead;
    }
    return Settle::Narrow;
}
} // namespace

std::optional<std::pair<Buffer*, u32>> BufferCache::ObtainSmallDirect(VAddr device_addr, u32 size,
                                                                     BufferId buffer_id,
                                                                     bool shader_read) {
    // bbport keeps copying small read-only ranges when they are bound (buffer_cache.cpp:1072-1077,
    // stream path :1161-1191): commands decoded later change them on the CPU, and a draw decoded
    // before must keep its values; its in-place reads (:1079-1112) rely on a residency
    // generation. Here a range is bound in its cached buffer only while the tracker sees every
    // page CPU-clean, i.e. unwritten since its upload into that buffer. A later CPU write makes
    // the page CPU-modified (write fault) before it lands, and the buffer only changes through
    // uploads recorded after this draw (with a barrier from every stage), so the draw reads the
    // values the stream copy would have taken.
    ScratchProfile::Cpu scratch_direct(38, "ObtainBuffer::SmallReadDirect");
    struct Stats {
        u64 candidates{}, hits{}, no_buffer{}, image_data{}, barrier_state{}, cpu_dirty{},
            verified{}, mismatches{};
        // Bit 44: its bindings (in hits too), sampled ones, flag cross-check failures, and the
        // barrier_state misses by Settle (with the bit off, Created and TransferRead would bind).
        u64 settled_created{}, settled_transfer{}, settle_verified{}, inconsistent{};
        std::array<u64, static_cast<size_t>(Settle::Count)> left{};
    };
    static thread_local Stats stats;
    if ((++stats.candidates & 0x3ffff) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST small_read_direct: hits={} misses={} (no_buffer={} image_data={} "
                 "barrier_state={} cpu_dirty={}) verified={} mismatches={} byte_mismatches={}",
                 stats.hits,
                 stats.no_buffer + stats.image_data + stats.barrier_state + stats.cpu_dirty,
                 stats.no_buffer, stats.image_data, stats.barrier_state, stats.cpu_dirty,
                 stats.verified, stats.mismatches,
                 g_small_direct_byte_mismatches.load(std::memory_order_relaxed));
        const auto left = [&](Settle s) { return stats.left[static_cast<size_t>(s)]; };
        LOG_INFO(Render_Vulkan,
                 "FAST small_read_settle: {} settled={} (created={} transfer_read={}) verified={} "
                 "byte_mismatches={} inconsistent={}; barrier_state left: would_settle={} "
                 "write={} narrow={} unseen={} fixed={}",
                 FastRender::On(SmallReadSettle) ? "on" : "off",
                 stats.settled_created + stats.settled_transfer, stats.settled_created,
                 stats.settled_transfer, stats.settle_verified,
                 g_small_settle_byte_mismatches.load(std::memory_order_relaxed),
                 stats.inconsistent, left(Settle::Created) + left(Settle::TransferRead),
                 left(Settle::Write), left(Settle::Narrow), left(Settle::Unseen),
                 left(Settle::Fixed));
    }
    // The buffer FindBuffer would return without creating one: the caller's (BindBuffers looked
    // it up first) while it is alive and covers the range, else the page table's.
    if (size == 0) {
        ++stats.no_buffer;
        return std::nullopt;
    }
    if (IsBufferInvalid(buffer_id) || !slot_buffers[buffer_id].IsInBounds(device_addr, size)) {
        buffer_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (IsBufferInvalid(buffer_id) || !slot_buffers[buffer_id].IsInBounds(device_addr, size)) {
            ++stats.no_buffer;
            return std::nullopt;
        }
    }
    Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.image_data) {
        ++stats.image_data;
        return std::nullopt;
    }
    // Buffer::GetBarrier's state. A descriptor's shader-read barrier must be a no-op (as for the
    // stream buffer: no barrier, no render-pass break), and the next write of the buffer must
    // still get a barrier whose source is every stage, so it waits for this read. A vertex or
    // index read asks for no barrier on a range that is not GPU-modified and leaves the state.
    // FAST_GPU_READ_BARRIERS' "visible to every read" state is settled for a shader read too
    // (Buffer::ReadsSettled): its GetBarrier asks for nothing there, and its stage is every stage.
    const vk::AccessFlags2 shader_read_access{vk::AccessFlagBits2::eShaderRead};
    const vk::AccessFlags2 shader_write_access{vk::AccessFlagBits2::eShaderWrite};
    const bool settled =
        buffer.stage == vk::PipelineStageFlagBits2::eAllCommands &&
        (shader_read ? (buffer.access_mask == shader_read_access || buffer.ReadsSettled())
                     : buffer.access_mask != shader_write_access);
    // Bit 44 (SmallReadSettle), shader reads only. The barrier BindBuffers would ask for is
    // needed only for writes not yet visible to shader reads (a read after a read needs none)
    // and to make the next write wait for this read. A buffer bound in place without it is set
    // to the state that barrier leaves (shader read at every stage), so every later request
    // other than a shader read still gets a barrier whose source is every stage, and a shader
    // read needs none. Taken:
    //  - Created: memory read and write at every stage, the state a buffer is made in or that
    //    JoinOverlap leaves after its copy's barrier. No write was asked for since (that changes
    //    the state); the writes GetBarrier does not see end with their own barrier into memory
    //    read at every stage. Buffer::shader_reads_visible is required as well.
    //  - TransferRead: a texture upload's read (TextureCache::RefreshImage) while every write
    //    is visible to shader reads (shader_reads_visible). Its next upload from this buffer
    //    then gets an execution-only barrier where it has already ended rendering.
    // Left: vertex, index and indirect read states (their next read would then get a barrier,
    // inside a render pass), states after a write access, writes not yet visible to shader
    // reads. No state is new to GetBarrier: the in-place path of a larger range would leave the
    // same one, with a barrier this argument shows to be a no-op. A sampled use's stream copy
    // (FAST_SMALL_READ_DIRECT_VERIFY) leaves the state as it was.
    Settle where = Settle::Fixed; // bit 44's reading of the state when bit 33's test failed
    bool by_settle = false;       // bound in place by bit 44
    if (!settled) {
        where = ClassifySettle(buffer, shader_read);
        by_settle = (where == Settle::Created || where == Settle::TransferRead) &&
                    FastRender::On(SmallReadSettle);
        if (!by_settle) {
            ++stats.barrier_state;
            ++stats.left[static_cast<size_t>(where)];
            return std::nullopt;
        }
    }
    if (!memory_tracker->IsRegionCpuCleanRelaxed(device_addr, size)) {
        ++stats.cpu_dirty;
        return std::nullopt;
    }
    const u32 verify_period = g_small_read_direct_verify_period;
    static thread_local u32 verify_tick = 0;
    if (verify_period != 0 && ++verify_tick % verify_period == 0) [[unlikely]] {
        // A sampled use takes the stream copy (the slow path). Its direct binding is checked: the
        // locked tracker and the page table now, the bytes once the GPU has read them.
        // Bit 44's bindings as well; the read-back (behind a full barrier) checks their bytes,
        // the barrier argument above their synchronisation. Two checks of that argument's
        // inputs for descriptors: BindResources' per-draw test (no buffer of this draw bound
        // for writing, which the resync memo's write mark would show), and the flag against bit
        // 33's state (shader read at every stage is entered only with a barrier that sets it).
        ++stats.verified;
        if (by_settle) {
            ++stats.settle_verified;
        }
        const bool cpu_modified = memory_tracker->IsRegionCpuModified(device_addr, size);
        const BufferId found = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        const bool written_this_draw =
            shader_read && buffer.perf_bound_draw == Vulkan::Perf::DrawSerial();
        if (settled && shader_read && !buffer.shader_reads_visible) {
            ++stats.inconsistent;
            if (stats.inconsistent <= 16 || (stats.inconsistent & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST small_read_settle verify: buffer {:#x} is in the shader-read "
                          "state but not marked visible to shader reads, inconsistency #{}",
                          buffer.CpuAddr(), stats.inconsistent);
            }
        }
        if (cpu_modified || found != buffer_id || written_this_draw) {
            ++stats.mismatches;
            if (stats.mismatches <= 64 || (stats.mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST small_read_direct verify: {:#x}+{} in buffer {:#x}: locked "
                          "tracker CPU-modified={}, page table buffer {} for {}, bound for "
                          "writing in this draw={}, bit 44={}, mismatch #{}",
                          device_addr, size, buffer.CpuAddr(), cpu_modified, found.index,
                          buffer_id.index, written_this_draw, by_settle, stats.mismatches);
            }
        } else {
            VerifySmallDirect(buffer, device_addr, size, shader_read, by_settle);
        }
        return std::nullopt;
    }
    if (by_settle) {
        // Bit 44: the state BindBuffers' shader-read GetBarrier would leave, without its
        // barrier (see above); that GetBarrier then asks for nothing.
        buffer.SettleShaderRead();
        if (where == Settle::Created) {
            ++stats.settled_created;
        } else {
            ++stats.settled_transfer;
        }
    }
    ++stats.hits;
    std::atomic_ref<u64>(buffer.last_used_tick).store(gc_tick, std::memory_order_relaxed);
    return std::pair<Buffer*, u32>{&buffer, static_cast<u32>(buffer.Offset(device_addr))};
}

bool BufferCache::SmallDirectCandidate(VAddr device_addr, u32 size) {
    if (!small_direct_supported || !FastRender::On(SmallReadDirect) || g_tracked_small_buffers ||
        size == 0 || size > CACHING_PAGESIZE) {
        return false;
    }
    // The buffer ObtainSmallDirect would find without a caller's id: the page table's.
    const BufferId buffer_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
    if (IsBufferInvalid(buffer_id) || !slot_buffers[buffer_id].IsInBounds(device_addr, size)) {
        return false;
    }
    return !slot_buffers[buffer_id].image_data &&
           memory_tracker->IsRegionCpuCleanRelaxed(device_addr, size);
}

void BufferCache::VerifySmallDirect(Buffer& buffer, VAddr device_addr, u32 size, bool both_eyes,
                                    bool by_settle) {
    // One comparison in flight at a time (as CheckSkippedResync): the bytes the binding would
    // read at this point of the command stream (and the right copy a descriptor is paired with
    // under native stereo) against the guest bytes the stream copy takes now.
    static std::atomic<bool> in_flight{false};
    if (in_flight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!small_direct_readback) {
        small_direct_readback = std::make_shared<Buffer>(
            instance, scheduler, MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eTransferDst,
            2 * CACHING_PAGESIZE);
    }
    const auto readback = small_direct_readback;
    std::vector<u8> guest(size);
    memory->CopySparseMemory(device_addr, guest.data(), size);
    const u64 offset = buffer.Offset(device_addr);
    const vk::Buffer right =
        both_eyes ? NativeStereo::ExistingRight(buffer.Handle()) : vk::Buffer{};
    const vk::MemoryBarrier2 to_transfer{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    scheduler.EndRendering(); // verification only
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_transfer,
    });
    cmdbuf.copyBuffer(buffer.Handle(), readback->Handle(), vk::BufferCopy{offset, 0, size});
    if (right) {
        cmdbuf.copyBuffer(right, readback->Handle(),
                          vk::BufferCopy{offset, CACHING_PAGESIZE, size});
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
    });
    const VAddr buffer_addr = buffer.CpuAddr();
    const u32 eyes = right ? 2u : 1u;
    scheduler.DeferOperation([readback, guest = std::move(guest), device_addr, size, buffer_addr,
                              eyes, by_settle] {
        if (readback->mapped_data.size() >= 2 * CACHING_PAGESIZE) {
            for (u32 eye = 0; eye < eyes; ++eye) {
                const u8* const bound = readback->mapped_data.data() + eye * CACHING_PAGESIZE;
                const auto [want, have] = std::mismatch(guest.begin(), guest.end(), bound);
                if (want == guest.end()) {
                    continue;
                }
                const u64 n =
                    g_small_direct_byte_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (by_settle) {
                    g_small_settle_byte_mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                if (n <= 64 || (n & 1023) == 0) {
                    LOG_ERROR(Render_Vulkan,
                              "FAST small_read_direct verify: {} eye of {:#x}+{} in buffer "
                              "{:#x} differs from guest memory at byte {} (bound {:#x}, guest "
                              "{:#x}), bit 44={}, byte mismatch #{}",
                              eye == 0 ? "left" : "right", device_addr, size, buffer_addr,
                              want - guest.begin(), *have, *want, by_settle, n);
                }
            }
        }
        in_flight.store(false, std::memory_order_release);
    });
}

BufferCache::ImageUploadBuffer BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    MaterializeRetiles(gpu_addr, size, RetileImageUpload);
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            NoteUsed(buffer_id);
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            return {&buffer, static_cast<u32>(buffer.Offset(gpu_addr)), {}};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (IsRegionGpuModified(gpu_addr, size)) {
        const auto [buffer, offset] = ObtainBuffer(gpu_addr, size, false, false);
        return {buffer, offset, {}};
    }
    // In all other cases, just do a CPU copy to the staging buffer.
    const auto staging = staging_pool.Request(size, instance.StorageMinAlignment());
    memory->CopySparseMemory(gpu_addr, staging.data, size);
    staging.Flush();
    return {staging.buffer, static_cast<u32>(staging.offset), staging};
}

BufferCache::ImageUploadBuffer BufferCache::ObtainBufferForBytes(const u8* data, u32 size) {
    const auto staging = staging_pool.Request(size, instance.StorageMinAlignment());
    std::memcpy(staging.data, data, size);
    staging.Flush();
    return {staging.buffer, static_cast<u32>(staging.offset), staging};
}

bool BufferCache::IsRegionRegistered(VAddr addr, size_t size) {
    // Check if we are missing some edge case here
    std::shared_lock lk{buffer_ranges_mutex}; // called from any thread (buffer_cache.h)
    return buffer_ranges.Intersects(addr, size);
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    ScratchProfile::Cpu scratch_dirty(26,"BufferCache::IsRegionGpuModified");
    return memory_tracker->IsRegionGpuModified(addr, size);
}

bool BufferCache::HasGpuWrites(VAddr addr, size_t size) const {
    return gpu_modified_ranges.Intersects(addr, size);
}

u64 BufferCache::TrackingEpoch(VAddr addr, u64 size) const noexcept {
    return memory_tracker->RangeEpoch(addr, size);
}

bool BufferCache::IsRangeFullyGpuModified(VAddr addr, u64 size) {
    return memory_tracker->IsRegionAllGpuModified(addr, size) &&
           gpu_modified_ranges.Contains(addr, size);
}

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size) {
    ScratchProfile::Cpu scratch_find(27,"BufferCache::FindBuffer");
    ASSERT(device_addr != 0);
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;
    if (!buffer_id) {
        return NoteUsed(CreateBuffer(device_addr, size));
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        return NoteUsed(buffer_id);
    }
    return NoteUsed(CreateBuffer(device_addr, size));
}

// Every way to a buffer stamps it (EvictIdleBuffers' idle age; VR_VRAM_REPORT's): lookups here,
// the vertex, index and copy memos' hits and image uploads (MarkUsed).
BufferId BufferCache::NoteUsed(BufferId buffer_id) {
    std::atomic_ref<u64>(slot_buffers[buffer_id].last_used_tick)
        .store(gc_tick, std::memory_order_relaxed);
    return buffer_id;
}

namespace {
// Experiment switch (logs-on builds): VR_BUFFER_GROW_FILE names a file read once a second; "1" in
// it turns geometric growth on, anything else (or no file) off. Release builds: off.
std::atomic<bool> g_buffer_grow{false};
} // namespace

bool BufferCache::BufferGrowGeometric() {
    return g_buffer_grow.load(std::memory_order_relaxed);
}

void BufferCache::PollBufferGrowSwitch() {
    if constexpr (Common::Log::Enabled) {
        static const char* const file = std::getenv("VR_BUFFER_GROW_FILE");
        static s64 checked_second = 0;
        if (file == nullptr) {
            return;
        }
        const s64 second = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (second == checked_second) {
            return;
        }
        checked_second = second;
        bool on = false;
        if (FILE* f = std::fopen(file, "r")) {
            on = std::fgetc(f) == '1';
            std::fclose(f);
        }
        if (g_buffer_grow.exchange(on, std::memory_order_relaxed) != on) {
            LOG_WARNING(Render_Vulkan, "Buffer growth: {} (VR_BUFFER_GROW_FILE)",
                        on ? "GEOMETRIC" : "2 MiB steps");
        }
    }
}

// FAST_BUFFER_LEAP_FIX ('0' off, on by default since 0.5.5; read once): the stream leap grows a region in
// the direction its requests go past the buffers they merge (it grew the side where the old
// buffer stuck out: a region streamed upward was re-created 16 KB at a time, 3.2 GB allocated
// in one sewers load, 2026-10-24), by the region's own size, and a leapt buffer keeps its score
// so the next growth leaps again at once.
static const bool g_leap_fix = [] {
    const char* value = std::getenv("FAST_BUFFER_LEAP_FIX");
    return !(value && value[0] == '0');
}();

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
    const VAddr request_begin = begin, request_end = end; // FAST_BUFFER_LEAP_FIX
    VAddr overlaps_begin = ~VAddr{0}, overlaps_end = 0;
    int stream_score = 0;
    bool has_stream_leap = false;
    const auto expand_begin = [&](VAddr add_value) {
        static constexpr VAddr min_page = CACHING_PAGESIZE + DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    const auto expand_end = [&](VAddr add_value) {
        static constexpr VAddr max_page = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.is_picked) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.is_picked = true;
        const VAddr overlap_device_addr = overlap.CpuAddr();
        overlaps_begin = std::min(overlaps_begin, overlap_device_addr);
        overlaps_end = std::max(overlaps_end, overlap_device_addr + overlap.SizeBytes());
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const VAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        // FAST_BUFFER_LEAP_FIX: a region leaps on its third growth, not its seventeenth.
        if (stream_score > (g_leap_fix ? 2 : STREAM_LEAP_THRESHOLD) && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            // Experiment (2026-10-18): grow by the region's own size (at most 128 MiB) instead of
            // 2 MiB. Streaming in an area re-created one region in 2 MiB steps: 2.7 GB allocated
            // in one frame for 589 MB asked, a 100 ms frame (the sewers).
            const VAddr leap = BufferGrowGeometric()
                                   ? std::clamp<VAddr>(end - begin, CACHING_PAGESIZE * 128, 128_MB)
                                   : CACHING_PAGESIZE * 128;
            // FAST_BUFFER_LEAP_FIX: the side the request goes past the buffers merged so far
            // (inside the scan, which then picks up whatever the leap now covers).
            const bool leap_right = g_leap_fix ? request_end > overlaps_end : expands_right;
            const bool leap_left = g_leap_fix ? request_begin < overlaps_begin : expands_left;
            if (leap_right) {
                expand_end(leap);
            }
            if (leap_left) {
                expand_begin(leap);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

bool BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                              bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    if (overlap.image_data) {
        new_buffer.image_data = true; // bit 33: the image data comes along with the copy
    }
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = dst_base_offset,
        .size = overlap.SizeBytes(),
    };
    if (new_buffer.sparse_view && overlap.sparse_view) {
        // Sparse views (SparsePool): the new buffer is bound to the old one's pieces, so its bytes
        // are already there. Only a right-eye copy, if the old buffer had one, still moves.
        NativeStereo::CopyRight(scheduler, overlap.Handle(), new_buffer.Handle(), std::span{&copy, 1});
        new_buffer.PerfNoteWrite();
        DeleteBuffer(overlap_id);
        return true;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.Commands();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> pre_barriers{};
    if (auto src_barrier = overlap.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                              vk::PipelineStageFlagBits2::eTransfer)) {
        pre_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier =
            new_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, dst_base_offset)) {
        pre_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    NativeStereo::Copy(scheduler, overlap.Handle(), new_buffer.Handle(), copy);
    new_buffer.PerfNoteWrite();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> post_barriers{};
    if (auto src_barrier =
            overlap.GetBarrier(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                               vk::PipelineStageFlagBits2::eAllCommands)) {
        post_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier = new_buffer.GetBarrier(
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eAllCommands, dst_base_offset)) {
        post_barriers.push_back(*dst_barrier);
        // Bit 44: this barrier makes the copy (and every earlier JoinOverlap copy, each behind its
        // own) visible to every read at every stage; nothing else wrote the new buffer yet. The
        // write access it asks for is only the state's: nothing writes after it here.
        new_buffer.shader_reads_visible = true;
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
        .pBufferMemoryBarriers = post_barriers.data(),
    });
    DeleteBuffer(overlap_id);
    return false;
}

// FAST_BUFFER_SPARSE_MIN_KB (read once; default 2048): a merge this large or larger makes a sparse
// view (SparsePool); a smaller one, and every first creation, stays an ordinary buffer, which is
// faster to make (a sparse buffer ~0.15 ms on the RTX 5080) and costs 16 KB pages, not 64 KB
// blocks. The pools that grew by copying are far larger than this.
// FAST_SPARSE_VERIFY=1 (a test, read once): VerifySparseMerge on every merge of sparse views.
static const bool g_sparse_verify = [] {
    const char* value = std::getenv("FAST_SPARSE_VERIFY");
    return value && value[0] == '1';
}();

static const u64 g_sparse_min_bytes = [] {
    const char* value = std::getenv("FAST_BUFFER_SPARSE_MIN_KB");
    return (value ? u64(std::strtoull(value, nullptr, 10)) : u64{2048}) << 10;
}();

namespace {
// Logs-on builds: CreateBuffer's time by step (its "create buffer" total hid which one costs, and
// logged the size asked for, not the size made); a call over 2 ms is logged with it.
struct CreateSteps {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now(), last = start;
    std::array<double, 4> ms{}; // overlaps, creation, merge, registration
    void Mark(size_t step) {
        if constexpr (Common::Log::Enabled) {
            const auto now = Clock::now();
            ms[step] += std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
        }
    }
};
} // namespace

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    Common::Slow::Scope slow{Common::Slow::CreateBuffer, wanted_size};
    CreateSteps steps;
    const u32 asked = wanted_size;
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    // Sparse views own whole 64 KB blocks (no block is shared with another buffer): a large
    // enough result, or one that takes in a view, is resolved again over whole blocks.
    bool view_wanted = false;
    if (sparse_pool.Enabled()) {
        bool takes_view = false;
        for (const BufferId id : overlap.ids) {
            takes_view |= slot_buffers[id].sparse_view;
        }
        // A merge makes a view (a buffer that grows: copy-on-grow is what views remove); a first
        // creation stays ordinary, as fast as before (a load makes hundreds of MB of them).
        view_wanted = takes_view ||
                      (!overlap.ids.empty() && overlap.end - overlap.begin >= g_sparse_min_bytes);
        const VAddr block_begin = SparseLogic::AlignDown(overlap.begin);
        const VAddr block_end = SparseLogic::AlignUp(overlap.end);
        if (view_wanted && (block_begin != overlap.begin || block_end != overlap.end) &&
            block_begin >= CACHING_PAGESIZE + DEVICE_PAGESIZE &&
            block_end - block_begin <= std::numeric_limits<u32>::max()) {
            for (const BufferId id : overlap.ids) {
                slot_buffers[id].is_picked = false;
            }
            overlap = ResolveOverlaps(block_begin, static_cast<u32>(block_end - block_begin));
        }
    }
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    steps.Mark(0);
    // Diagnostic (VR_BUFFER_GROWTH_LOG=1): creations that merge buffers or exceed 8 MB.
    static const bool growth_log = std::getenv("VR_BUFFER_GROWTH_LOG") != nullptr;
    if (growth_log && (!overlap.ids.empty() || size > (8u << 20))) {
        static u32 logged = 0;
        u64 merged = 0;
        for (const BufferId id : overlap.ids)
            merged += slot_buffers[id].SizeBytes();
        if (logged++ < 400)
            LOG_INFO(Render_Vulkan,
                     "VR buffer growth: wanted {:#x}+{:#x} -> {:#x}+{:#x} ({} KB), merging {} "
                     "buffers ({} KB), stream leap {}",
                     device_addr, wanted_size, overlap.begin, size, size >> 10,
                     overlap.ids.size(), merged >> 10, overlap.has_stream_leap);
    }
    // A sparse view over whole blocks, when VRAM had the slab for it; an ordinary buffer otherwise.
    vk::Buffer view{};
    std::vector<u32> view_pieces;
    if (view_wanted && overlap.begin % SparseLogic::BlockSize == 0 &&
        size % SparseLogic::BlockSize == 0) {
        Common::Slow::Scope slow_view{Common::Slow::SparseView, size};
        view = sparse_pool.CreateView(overlap.begin, size,
                                      AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                      view_pieces);
    }
    const BufferId new_buffer_id =
        view ? slot_buffers.insert(instance, scheduler, overlap.begin, u64{size}, view, &sparse_pool,
                                   std::move(view_pieces))
             : slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin,
                                   AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
    steps.Mark(1);
    bool shared = false;
    // FAST_SPARSE_VERIFY: the old views merged without a copy (their handles live until the tick).
    boost::container::small_vector<std::tuple<vk::Buffer, u64, u64>, 8> verify;
    for (const BufferId overlap_id : overlap.ids) {
        const Buffer& old = slot_buffers[overlap_id];
        const auto old_view = std::make_tuple(old.Handle(), u64{old.SizeBytes()},
                                              u64{old.CpuAddr() - overlap.begin});
        // FAST_BUFFER_LEAP_FIX: a leapt region keeps its score (the next growth leaps again).
        const bool joined =
            JoinOverlap(new_buffer_id, overlap_id, g_leap_fix || !overlap.has_stream_leap);
        if (joined && g_sparse_verify) {
            verify.push_back(old_view);
        }
        shared |= joined;
    }
    if (shared) {
        // The old handles' writes reach the pieces the new buffer binds: one barrier orders them
        // before whatever uses the new handle (the copies a merge made had theirs).
        scheduler.EndRendering();
        const vk::MemoryBarrier2 barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        });
        // Bit 44: every write so far is visible to every read now, as after a merge's copy.
        slot_buffers[new_buffer_id].shader_reads_visible = true;
        for (const auto& [old_view, bytes, offset] : verify) {
            VerifySparseMerge(old_view, bytes, slot_buffers[new_buffer_id].Handle(), offset);
        }
    }
    steps.Mark(2);
    Register(new_buffer_id);
    steps.Mark(3);
    if constexpr (Common::Log::Enabled) {
        const double total =
            std::chrono::duration<double, std::milli>(CreateSteps::Clock::now() - steps.start)
                .count();
        static u32 logged = 0;
        if (total >= 2.0 && logged < 300) {
            ++logged;
            LOG_WARNING(Render_Vulkan,
                        "SLOW create buffer: {:.2f} ms: overlaps {:.2f}, {} {:.2f}, merge {:.2f} "
                        "({} buffers), register {:.2f}; asked {} KB, made {} KB",
                        total, steps.ms[0], view ? "sparse view" : "buffer", steps.ms[1],
                        steps.ms[2], overlap.ids.size(), steps.ms[3], asked >> 10, size >> 10);
        }
    }
    return new_buffer_id;
}

void BufferCache::VerifySparseMerge(vk::Buffer old_view, u64 bytes, vk::Buffer new_view,
                                    u64 offset) {
    // The first and the last MiB of the old view (a wrong bind shows at the edges of runs).
    static constexpr u64 Sample = 1_MB;
    std::array<std::pair<u64, u64>, 2> parts{{{0, std::min(bytes, Sample)}, {0, 0}}};
    if (bytes > Sample) {
        const u64 tail = std::min(Sample, bytes - Sample);
        parts[1] = {bytes - tail, tail};
    }
    u64 total = 0;
    for (const auto& [at, size] : parts) {
        total += size;
    }
    auto readback = std::make_shared<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                             vk::BufferUsageFlagBits::eTransferDst, 2 * total);
    boost::container::small_vector<vk::BufferCopy, 2> from_old, from_new;
    u64 at_out = 0;
    for (const auto& [at, size] : parts) {
        if (size == 0) {
            continue;
        }
        from_old.push_back({at, at_out, size});
        from_new.push_back({offset + at, total + at_out, size});
        at_out += size;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.copyBuffer(old_view, readback->Handle(), from_old);
    cmdbuf.copyBuffer(new_view, readback->Handle(), from_new);
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &to_host});
    scheduler.DeferOperation([readback, total, bytes, offset] {
        static std::atomic<u64> checked{0}, bad{0};
        ++checked;
        const u8* const data = readback->mapped_data.data();
        if (std::memcmp(data, data + total, total) != 0) {
            ++bad;
            LOG_WARNING(Render_Vulkan,
                        "Sparse verify: a merged view ({} KB at +{:#x}) differs from the new "
                        "view's bytes: {} of {} merges bad",
                        bytes >> 10, offset, bad.load(), checked.load());
        } else if ((checked & 63) == 0) {
            LOG_INFO(Render_Vulkan, "Sparse verify: {} merges checked, {} bad", checked.load(),
                     bad.load());
        }
    });
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager.ProcessFaultBuffer();
}

void BufferCache::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
}

void BufferCache::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId buffer_id) {
    copy_memo_epoch.fetch_add(1, std::memory_order_acq_rel); // perf_copy_memo
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    const VAddr device_addr_begin = buffer.CpuAddr();
    const VAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    const u64 size_pages = page_end - page_begin;
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page].buffer_id = buffer_id;
        } else {
            page_table[page].buffer_id = BufferId{};
        }
    }
    if constexpr (insert) {
        total_used_memory += Common::AlignUp(size, CACHING_PAGESIZE);
        buffer.SetLRUId(lru_cache.Insert(buffer_id, gc_tick));
        // The page table's device addresses, only when the DMA table exists (a review: the array
        // was built for every buffer, a large merge's thousands of pages included).
        if (DmaTable::allocated.load(std::memory_order_relaxed)) {
            boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
            bda_addrs.reserve(size_pages);
            for (u64 i = 0; i < size_pages; ++i) {
                vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
                bda_addrs.push_back(addr);
            }
            WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                            bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        }
        std::unique_lock lk{buffer_ranges_mutex};
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        total_used_memory -= Common::AlignUp(size, CACHING_PAGESIZE);
        lru_cache.Free(buffer.LRUId());
        if (DmaTable::allocated.load(std::memory_order_relaxed)) {
            const u64 offset =
                bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
            bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
        }
        std::unique_lock lk{buffer_ranges_mutex};
        buffer_ranges.Subtract(buffer.CpuAddr(), buffer.SizeBytes());
    }
}

namespace TransferEyeBarriers = Vulkan::TransferEyeBarriers;

// FAST_TRANSFER_EYE_BARRIERS_VERIFY (vk_transfer_eye_barriers.h): both eyes copied the same
// staging bytes into `left` and `right`; read both destination ranges back after the
// post-barrier and compare them once the GPU has finished. Recording thread; one in flight.
static void VerifyTransferEyeCopies(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                                    vk::Buffer left, vk::Buffer right,
                                    std::span<const vk::BufferCopy> copies) {
    using namespace Vulkan::TransferEyeBarriers;
    auto& stats = g_stats;
    vk::DeviceSize total = 0;
    for (const auto& copy : copies) {
        total += copy.size;
    }
    constexpr vk::DeviceSize MaxBytes = 4ull << 20;
    if (total == 0 || total > MaxBytes) {
        stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        stats.verify_in_flight.store(false, std::memory_order_release);
        return;
    }
    // Leaked on purpose: a static owner would destroy them after the device at exit.
    static Buffer* left_back = nullptr;
    static Buffer* right_back = nullptr;
    if (!left_back) {
        left_back = new Buffer(instance, scheduler, MemoryUsage::Download, 0,
                               vk::BufferUsageFlagBits::eTransferDst, MaxBytes);
        right_back = new Buffer(instance, scheduler, MemoryUsage::Download, 0,
                                vk::BufferUsageFlagBits::eTransferDst, MaxBytes);
    }
    boost::container::small_vector<vk::BufferCopy, 8> left_reads, right_reads;
    vk::DeviceSize packed = 0;
    for (const auto& copy : copies) {
        left_reads.push_back({copy.dstOffset, packed, copy.size});
        packed += copy.size;
    }
    right_reads = left_reads;
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.copyBuffer(left, left_back->Handle(), left_reads);
    cmdbuf.copyBuffer(right, right_back->Handle(), right_reads);
    // Order the readback before any later writer of left/right, and expose it to the host.
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    scheduler.DeferOperation([a = left_back, b = right_back, total,
                              allocator = instance.GetAllocator()] {
        auto& stats = g_stats;
        for (Buffer* back : {a, b}) {
            if (!back->is_coherent) { // host-cached download memory: see the GPU's writes
                vmaInvalidateAllocation(allocator, back->buffer.allocation, 0, total);
            }
        }
        if (a->mapped_data.size() < total || b->mapped_data.size() < total) {
            stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        } else if (std::memcmp(a->mapped_data.data(), b->mapped_data.data(), total) == 0) {
            stats.verify_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
            u64 differing = 0, first = total;
            for (vk::DeviceSize i = 0; i < total; ++i) {
                if (a->mapped_data[i] != b->mapped_data[i]) {
                    first = std::min<u64>(first, i);
                    ++differing;
                }
            }
            const u64 n = stats.verify_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || n % 256 == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST transfer_eye_barriers verify: {} of {} bytes differ between the "
                          "left and right upload destinations, first at packed byte {}, "
                          "mismatch #{}",
                          differing, total, first, n);
            }
        }
        stats.verify_in_flight.store(false, std::memory_order_release);
    });
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                                    bool is_texel_buffer, bool retire_cpu_uploads) {
    ScratchProfile::Cpu scratch_cpu(1,"BufferCache::SynchronizeBuffer");
    // Every upload and every GPU write it precedes (ObtainBuffer, CopyBuffer) sees the ranges the
    // game handed out again before it as dead GPU data.
    TakeFresh();
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    std::optional<UploadStagingPool::Allocation> upload;
    SCOPE_EXIT {
        if (upload) {
            upload->Retire();
        }
    };
    const bool async_upload = FastRender::On(FastRender::CopyThreads) && !is_written;
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 device_addr_out, u64 range_size) {
            // FAST_GPU_PAGE_KEEP: a kept page uploads only what the CPU changed in its
            // GPU-written bytes (MemoryTracker::ForUpload).
            memory_tracker->ForUpload(
                device_addr_out, range_size,
                [&](VAddr address, u64 bytes) {
                    Common::MemoryWriteTrace::Event("buffer.upload.range", address, bytes, scheduler.CurrentTick());
                    copies.emplace_back(total_size_bytes, address - buffer_start, bytes);
                    total_size_bytes += bytes;
                },
                [this](VAddr page, u64 bytes, auto&& range) {
                    gpu_modified_ranges.ForEachInRange(page, bytes, range);
                },
                [](VAddr start, VAddr stop, MemoryTracker::LostWhy why) {
                    if (start < stop) {
                        Common::MemoryWriteTrace::Event("gpu.keep.replaced", start,
                                                       stop - start, static_cast<u8>(why));
                    }
                });
        },
        [&](bool safe_async) { upload = UploadCopies(buffer, copies, total_size_bytes, safe_async); },
        async_upload);

    const vk::Buffer src_buffer = upload ? upload->buffer->Handle() : vk::Buffer{};
    if (src_buffer) {
        scheduler.EndRendering();
    ScratchProfile::Gpu scratch_gpu(scheduler,11);
        const auto cmdbuf = scheduler.Commands();
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eTransferRead |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        const vk::BufferMemoryBarrier2 post_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        // FAST bit 63 (vk_transfer_eye_barriers.h): name the right twin in these barriers and
        // let NativeStereo::Copy drop its two global barriers between the eyes.
        vk::Buffer outer_right{};
        if (NativeStereo::Enabled() && TransferEyeBarriers::On()) {
            using namespace TransferEyeBarriers;
            static const bool scoped_env =
                std::getenv("VR_NATIVE_COPY_TRANSFER_BARRIERS") != nullptr;
            const vk::Buffer right_dst = NativeStereo::ExistingRight(buffer.Handle());
            const vk::Buffer right_src = NativeStereo::ExistingRight(src_buffer);
            if (!right_dst) {
                if (right_src) {
                    NoteFallback(Fallback::LazyTwin);
                }
            } else if (right_src) {
                NoteFallback(Fallback::RightSource);
            } else if (scoped_env) {
                NoteFallback(Fallback::EnvOverride);
            } else if (right_dst == buffer.Handle() || right_dst == src_buffer ||
                       src_buffer == buffer.Handle()) {
                NoteFallback(Fallback::Alias);
            } else {
                outer_right = right_dst;
            }
        }
        if (!outer_right) {
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &pre_barrier,
            });
            NativeStereo::Copy(scheduler, src_buffer, buffer.buffer, copies);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &post_barrier,
            });
        } else {
            std::array<vk::BufferMemoryBarrier2, 2> pre_barriers{pre_barrier, pre_barrier};
            std::array<vk::BufferMemoryBarrier2, 2> post_barriers{post_barrier, post_barrier};
            for (auto* barrier : {&pre_barriers[1], &post_barriers[1]}) {
                barrier->buffer = outer_right;
                barrier->size = VK_WHOLE_SIZE; // the twin is registered with its own size
            }
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 2,
                .pBufferMemoryBarriers = pre_barriers.data(),
            });
            NativeStereo::Copy(scheduler, src_buffer, buffer.buffer, copies, false, outer_right);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 2,
                .pBufferMemoryBarriers = post_barriers.data(),
            });
            TransferEyeBarriers::NoteTaken(TransferEyeBarriers::Site::Sync);
            if (TransferEyeBarriers::ClaimVerify()) {
                VerifyTransferEyeCopies(instance, scheduler, buffer.Handle(), outer_right,
                                        copies);
            }
        }
        // Motion snapshots need byte ownership, not the historical GPU-dirty page bit.
        // Only ranges the normal upload path actually emitted become CPU-authoritative;
        // untouched GPU bytes on mixed pages were excluded by ForUpload and stay owned.
        // Every upload does it (FAST_UPLOAD_RETIRE): when another pass uploaded a reloaded
        // model's indices before the motion read, they stayed marked GPU-written for good, and
        // after many areas dozens of characters' indices went through GPU snapshots each frame.
        if (retire_cpu_uploads || g_upload_retire) {
            bool retired = retire_cpu_uploads;
            for (const auto& copy : copies) {
                const VAddr at = buffer_start + copy.dstOffset;
                if (retire_cpu_uploads || gpu_modified_ranges.Intersects(at, copy.size)) {
                    gpu_modified_ranges.Subtract(at, copy.size);
                    retired = true;
                }
            }
            if (retired) {
                copy_memo_epoch.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        TouchBuffer(buffer);
        buffer.PerfNoteWrite(); // the resync memo: guest data uploaded into it
        for (const auto& copy : copies) {
            NoteMotionWrite(buffer_start + copy.dstOffset, copy.size);
        }
    }
    if (is_written) {
        // Bound for writing (a shader, a copy, a fill): written from here (a shader's writes come
        // with its draw: no memo is kept for this buffer during this draw's bindings).
        buffer.PerfNoteWriteBound();
        NoteMotionWrite(device_addr, size);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeBufferFromImage(buffer, device_addr, size);
    }
    return false;
}

std::optional<UploadStagingPool::Allocation> BufferCache::UploadCopies(
    Buffer& buffer, std::span<vk::BufferCopy> copies, size_t total_size_bytes, bool async_upload) {
    if (copies.empty()) {
        return {};
    }
    const auto staging = staging_pool.Request(total_size_bytes);
    // Eligibility and source protection still come from MemoryTracker. Select
    // coherence per allocation, since the pool may span different memory types.
    async_upload &= staging.buffer->is_coherent;
    for (auto& copy : copies) {
        u8* const destination = staging.data + copy.srcOffset;
        const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
        if (async_upload) {
            memory->QueueSparseMemory(device_addr, destination, copy.size);
        } else {
            memory->CopySparseMemory(device_addr, destination, copy.size);
        }
        copy.srcOffset += staging.offset;
    }
    staging.Flush();
    return staging;
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    ScratchProfile::Cpu scratch_cpu(4,"BufferCache::SynchronizeBufferFromImage");
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            buffer.image_data = true; // bit 33: no longer guest bytes there
            buffer.Fill(buffer.Offset(device_addr), size, ZmaskUncompressed);
            NoteMotionWrite(device_addr, size);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u32 buf_offset = buffer.Offset(image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buf_offset + mip_info.offset + mip_info.size > buffer.SizeBytes()) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (copy_size == 0) {
        return false;
    }
    // Bit 33: from here the buffer holds (or, on a memo skip, already held) the image's data.
    buffer.image_data = true;
    auto& tile_manager = texture_cache.GetTileManager();

    // vr.ini perf_resync_memo (vr_perf.h): the same image into the same bytes of the same buffer,
    // neither written since the last full resync (their write counters unchanged, no toggle or
    // raw-memory shader in between): the buffer still holds exactly what this resync would write.
    const PerfLogic::ResyncKey key{
        .image_uid = image.image_uid,
        .vk_image = reinterpret_cast<u64>(static_cast<VkImage>(image.GetImage())),
        .buffer_serial = buffer.perf_serial,
        .vk_buffer = reinterpret_cast<u64>(static_cast<VkBuffer>(buffer.Handle())),
        .buf_offset = buf_offset,
        .copy_size = copy_size,
        .num_copies = static_cast<u32>(buffer_copies.size()),
        .tiled = image.info.props.is_tiled ? 1u : 0u,
    };
    const PerfLogic::ResyncSeqs seqs_before{
        .image = image.perf_write_seq,
        .buffer = buffer.perf_write_seq,
        .global = Vulkan::Perf::GlobalSeq(),
    };
    const bool can_skip = PerfLogic::CanSkip(image.perf_resync, key, seqs_before);
    auto& count = Vulkan::Perf::Count();
    if (Vulkan::Perf::Probe()) [[unlikely]] {
        const bool img_same = !can_skip && image.perf_resync.valid &&
                              image.perf_resync.key == key &&
                              image.perf_resync.seqs.image == seqs_before.image &&
                              image.perf_resync.seqs.global == seqs_before.global;
        Vulkan::Perf::NoteResync(
            {.address = image.info.guest_address,
             .width = image.info.size.width,
             .height = image.info.size.height,
             .format = static_cast<u32>(image.info.pixel_format),
             .bits = image.info.num_bits,
             .size = static_cast<u32>(image.info.guest_size),
             .tiled = image.info.props.is_tiled != 0,
             .reader = Vulkan::Perf::Context(),
             .writer = image.perf_write_ctx,
             .draws_ago = Vulkan::Perf::DrawSerial() - image.perf_write_draw},
            can_skip ? (Vulkan::Perf::ResyncMemo() ? Vulkan::Perf::ResyncOutcome::Skipped
                                                   : Vulkan::Perf::ResyncOutcome::SkippableOff)
            : img_same ? Vulkan::Perf::ResyncOutcome::ImageSameBufferWritten
                       : Vulkan::Perf::ResyncOutcome::Full);
        static u32 probes = 0;
        if (probes < 12) {
            ++probes;
            const bool same_target = image.perf_resync.valid && image.perf_resync.key == key;
            LOG_INFO(Render_Vulkan,
                     "VR perf probe: resync #{} image {:#x} {} B {}x{}x{} mips {} layers {} fmt "
                     "{}/{} bits {} tiled {} mode {} uid {} -> buffer {:#x}+{} ({} B, {} copies); "
                     "reader {:#x}, last writer {:#x} {} draws ago; same target {}, image written "
                     "{}, buffer written {}, global {} -> {}",
                     probes, image.info.guest_address, image.info.guest_size,
                     image.info.size.width, image.info.size.height, image.info.size.depth,
                     image.info.resources.levels, image.info.resources.layers,
                     static_cast<u32>(image.info.pixel_format), vk::to_string(image.info.pixel_format),
                     image.info.num_bits, image.info.props.is_tiled ? 1 : 0,
                     static_cast<u32>(image.info.tile_mode), image.image_uid, buffer.CpuAddr(),
                     buf_offset, copy_size, buffer_copies.size(), Vulkan::Perf::Context(),
                     image.perf_write_ctx, Vulkan::Perf::DrawSerial() - image.perf_write_draw,
                     same_target, image.perf_resync.seqs.image != seqs_before.image,
                     image.perf_resync.seqs.buffer != seqs_before.buffer,
                     image.perf_resync.seqs.global != seqs_before.global,
                     can_skip ? "skippable" : "full");
        }
    }
    if (can_skip) {
        if (Vulkan::Perf::ResyncMemo()) {
            count.skipped.fetch_add(1, std::memory_order_relaxed);
            count.skip_bytes.fetch_add(copy_size, std::memory_order_relaxed);
            if (Vulkan::Perf::Check()) [[unlikely]] {
                CheckSkippedResync(image, buffer_copies, buffer, buf_offset, copy_size);
            }
            return true;
        }
        count.skippable_off.fetch_add(1, std::memory_order_relaxed);
    } else if (image.perf_resync.valid && image.perf_resync.key == key &&
               image.perf_resync.seqs.image == seqs_before.image &&
               image.perf_resync.seqs.global == seqs_before.global) {
        count.img_same_buf_written.fetch_add(1, std::memory_order_relaxed);
    }
    tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
    buffer.PerfNoteWrite(); // this resync wrote it
    NoteMotionWrite(buffer.CpuAddr() + buf_offset, copy_size);
    count.resyncs.fetch_add(1, std::memory_order_relaxed);
    count.resync_bytes.fetch_add(copy_size, std::memory_order_relaxed);
    // The memo, unless a shader bound in this draw writes the image or the buffer (or reads raw
    // memory): its writes land after this resync on the GPU.
    const u64 draw = Vulkan::Perf::DrawSerial();
    const bool written_this_draw = image.perf_write_draw == draw ||
                                   buffer.perf_bound_draw == draw ||
                                   Vulkan::Perf::DmaDraw() == draw;
    PerfLogic::Store(image.perf_resync, key,
                     {.image = image.perf_write_seq,
                      .buffer = buffer.perf_write_seq,
                      .global = Vulkan::Perf::GlobalSeq()},
                     written_this_draw);
    return true;
}

void BufferCache::CheckSkippedResync(Image& image, std::span<vk::BufferImageCopy> buffer_copies,
                                     Buffer& buffer, u32 buf_offset, u32 copy_size) {
    // VR_PERF_CHECK: the resync done the full way into a scratch buffer, both that and the
    // buffer's bytes read back, compared once the GPU is done (the frame keeps the skip's bytes).
    // One comparison in flight at a time, its three buffers kept and reused (a skip while one is
    // in flight is not checked): ~20 MB of scratch at 2720x1808, not 60 MB per skip.
    const u64 avail = buffer.SizeBytes() - buf_offset;
    const u64 cmp_size =
        image.info.props.is_tiled ? std::min<u64>(image.info.guest_size, avail) : copy_size;
    if (cmp_size == 0) {
        return;
    }
    static std::atomic<bool> in_flight{false};
    if (in_flight.load(std::memory_order_acquire)) {
        return;
    }
    struct CheckBuffers {
        std::shared_ptr<Buffer> scratch, have, want;
    };
    static CheckBuffers keep{};
    const u64 scratch_size = std::max<u64>(image.info.guest_size, cmp_size);
    if (!keep.scratch || keep.scratch->SizeBytes() < scratch_size) {
        keep.scratch = std::make_shared<Buffer>(
            instance, scheduler, MemoryUsage::DeviceLocal, 0,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst,
            scratch_size);
    }
    if (!keep.have || keep.have->SizeBytes() < cmp_size) {
        keep.have = std::make_shared<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                             vk::BufferUsageFlagBits::eTransferDst, cmp_size);
        keep.want = std::make_shared<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                             vk::BufferUsageFlagBits::eTransferDst, cmp_size);
    }
    in_flight.store(true, std::memory_order_release);
    const auto scratch = keep.scratch, have = keep.have, want = keep.want;
    const vk::MemoryBarrier2 all{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    const auto barrier = [&] {
        scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &all,
        });
    };
    scheduler.EndRendering();
    barrier();
    // What the skip leaves in the buffer.
    scheduler.Commands().copyBuffer(buffer.Handle(), have->Handle(),
                                         vk::BufferCopy{buf_offset, 0, cmp_size});
    // What the full resync would write (into the scratch buffer at offset 0).
    std::vector<vk::BufferImageCopy> copies(buffer_copies.begin(), buffer_copies.end());
    texture_cache.GetTileManager().TileImage(image, copies, scratch->Handle(), 0,
                                             static_cast<u32>(cmp_size));
    barrier();
    scheduler.Commands().copyBuffer(scratch->Handle(), want->Handle(),
                                         vk::BufferCopy{0, 0, cmp_size});
    const vk::MemoryBarrier2 host{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host,
    });
    Vulkan::Perf::Count().checks.fetch_add(1, std::memory_order_relaxed);
    const VAddr addr = image.info.guest_address;
    const u32 w = image.info.size.width, h = image.info.size.height;
    scheduler.DeferOperation([scratch, have, want, cmp_size, addr, w, h]() {
        if (have->mapped_data.size() >= cmp_size && want->mapped_data.size() >= cmp_size) {
            const auto c = PerfLogic::CompareBytes(
                std::span<const u8>{have->mapped_data.data(), cmp_size},
                std::span<const u8>{want->mapped_data.data(), cmp_size});
            if (c.bytes != 0) {
                Vulkan::Perf::Count().mismatches.fetch_add(1, std::memory_order_relaxed);
                const auto details = fmt::format(
                    "resync of image {:#x} {}x{}: {} of {} bytes differ, first at {}", addr, w, h,
                    c.bytes, cmp_size, c.first);
                Vulkan::Perf::ReportMismatch("resync memo", details.c_str());
            }
        }
        in_flight.store(false, std::memory_order_release);
    });
}

void BufferCache::SynchronizeBuffersInRange(VAddr device_addr, u64 size) {
    MaterializeRetiles(device_addr, size, RetileRange);
    const VAddr device_addr_end = device_addr + size;
    ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
        RENDERER_TRACE;
        VAddr start = std::max(buffer.CpuAddr(), device_addr);
        VAddr end = std::min(buffer.CpuAddr() + buffer.SizeBytes(), device_addr_end);
        u32 size = static_cast<u32>(end - start);
        SynchronizeBuffer(buffer, start, size, false, false);
    });
}

void BufferCache::WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes) {
    ScratchProfile::Cpu scratch_cpu(2,"BufferCache::WriteDataBuffer");
    vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = buffer.Offset(address),
        .size = num_bytes,
    };
    const auto staging = staging_pool.Request(num_bytes);
    SCOPE_EXIT {
        staging.Retire();
    };
    std::memcpy(staging.data, value, num_bytes);
    copy.srcOffset = staging.offset;
    staging.Flush();
    const vk::Buffer src_buffer = staging.buffer->Handle();
    scheduler.EndRendering();
    ScratchProfile::Gpu scratch_gpu(scheduler,12);
    const auto cmdbuf = scheduler.Commands();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    NativeStereo::Copy(scheduler, src_buffer, buffer.Handle(), copy);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    buffer.PerfNoteWrite();
    NoteMotionWrite(address, num_bytes);
}

// The catch-all log of GPU-written bytes an upload replaced with the guest's (MemoryTracker::
// ForUpload `lost`): each 4 KB page once (and every 64th time after), with why and when, so the
// path that lost the ghost triangles' data shows in the log. A page the CPU really rewrote (memory
// reused) shows here too; the face's pages are the ones replaced right before the triangles.
void BufferCache::NoteGpuBytesReplaced(VAddr start, VAddr stop, u8 why_code) {
    if (start >= stop) {
        return;
    }
    const auto why = static_cast<MemoryTracker::LostWhy>(why_code);
    const VAddr page = Common::AlignDown(start, 4096);
    const u64 total = replaced_total.fetch_add(1, std::memory_order_relaxed) + 1;
    // The page's slot (linear probing, 8 tries): its count of replacements.
    u64 seen = 0;
    size_t slot = static_cast<size_t>((page >> 12) * 0x9E3779B97F4A7C15ull >> 48) &
                  (ReplacedSlots - 1);
    for (int probe = 0; probe < 8; ++probe, slot = (slot + 1) & (ReplacedSlots - 1)) {
        auto& key = (*replaced_pages)[slot];
        u64 current = key.load(std::memory_order_relaxed);
        if (current == 0 &&
            key.compare_exchange_strong(current, page, std::memory_order_relaxed)) {
            current = page;
        }
        if (current == page) {
            seen = (*replaced_counts)[slot].fetch_add(1, std::memory_order_relaxed) + 1;
            break;
        }
    }
    if (seen == 0 ? (total & 63) != 0 : (seen != 1 && (seen & 63) != 0)) {
        return;
    }
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                   replaced_t0)
                         .count();
    LOG_INFO(Render_Vulkan,
             "FAST_GPU_PAGE_KEEP: REPLACED {} GPU-written bytes at {:#x}..{:#x} with the guest's "
             "({}) | page {:#x} x{} | t={:.1f} s, {} replacements, pool evictions {}",
             stop - start, start, stop, MemoryTracker::LostName(why), page, seen, t, total,
             memory_tracker->KeptPoolFull());
}

namespace {
// Off unless FAST_GPU_SETTLE=1 (2026-10-16: faceless faces broke without any upload replacing
// GPU bytes, so this did not fix them, and the one device loss of the night came in its run).
const bool g_gpu_settle = [] {
    const char* env = std::getenv("FAST_GPU_SETTLE");
    return env != nullptr && env[0] == '1';
}();
constexpr u64 SettleFrames = 45;               // about half a second without a GPU write
constexpr u64 SettleScanEvery = 8;             // frames between scans
constexpr u64 SettleBudget = 8ull << 20;       // bytes copied back per scan at most
constexpr u64 SettleMaxRange = 8ull << 20;     // larger ranges stay GPU-only (32 MB staging)
constexpr size_t SettleMaxEntries = 1u << 16;  // a runaway table is dropped, never grown
} // namespace

void BufferCache::NoteGpuWrite(VAddr addr, u64 size) {
    Common::MemoryWriteTrace::Event("gpu.write.range", addr, size, scheduler.CurrentTick());
    if (!g_gpu_settle || size == 0) {
        return;
    }
    std::scoped_lock lock{settle_mutex};
    auto& write = settle_writes[addr];
    write.size = std::max(write.size, size);
    write.tick = gc_tick;
}

void BufferCache::RequestReadback(VAddr addr, u64 size) {
    std::scoped_lock lock{settle_mutex};
    readback_requests.emplace_back(addr, size);
}

void BufferCache::SettleGpuWrites() {
    {
        std::vector<std::pair<VAddr, u64>> requests;
        {
            std::scoped_lock lock{settle_mutex};
            requests.swap(readback_requests);
        }
        for (const auto& [addr, size] : requests) {
            const bool gpu = IsRegionRegistered(addr, size) && IsRegionGpuModified(addr, size);
            LOG_INFO(Render_Vulkan, "VR draws: gpumem {:#x}+{}: {}", addr, size,
                     gpu ? "GPU-written, copied back" : "not GPU-written (guest memory as is)");
            if (gpu) {
                MaterializeRetiles(addr, size, RetileDownload);
                Buffer& buffer = slot_buffers[FindBuffer(addr, static_cast<u32>(size))];
                DownloadBufferMemory<true>(buffer, addr, size);
            }
        }
    }
    if (!g_gpu_settle || gc_tick % SettleScanEvery != 0) {
        return;
    }
    boost::container::small_vector<std::pair<VAddr, u64>, 16> ready;
    {
        std::scoped_lock lock{settle_mutex};
        u64 bytes = 0;
        for (auto it = settle_writes.begin(); it != settle_writes.end();) {
            if (it->second.size > SettleMaxRange) {
                it = settle_writes.erase(it); // render targets and big scratch: not ours
            } else if (gc_tick - it->second.tick >= SettleFrames &&
                       bytes + it->second.size <= SettleBudget) {
                ready.emplace_back(it->first, it->second.size);
                bytes += it->second.size;
                it = settle_writes.erase(it);
            } else {
                ++it;
            }
        }
        if (settle_writes.size() > SettleMaxEntries) {
            settle_writes.clear();
        }
    }
    u64 copied = 0;
    u32 ranges = 0;
    for (const auto& [addr, size] : ready) {
        if (!IsRegionRegistered(addr, size) || !IsRegionGpuModified(addr, size)) {
            continue; // unmapped since, or already back (a readback, the GC)
        }
        MaterializeRetiles(addr, size, RetileDownload);
        Buffer& buffer = slot_buffers[FindBuffer(addr, static_cast<u32>(size))];
        DownloadBufferMemory<true>(buffer, addr, size);
        copied += size;
        ++ranges;
    }
    if (ranges != 0) {
        const u64 n = settle_logs++;
        if (n < 32 || (n & 63) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST_GPU_SETTLE: {} GPU-written ranges ({} KB) untouched for {} frames "
                     "copied back to guest memory ({} scans so far)",
                     ranges, copied >> 10, SettleFrames, n + 1);
        }
    }
}

// FAST_BUFFER_EVICT=0 turns the buffer cache's eviction off (read once).
static const bool g_buffer_evict = [] {
    const char* value = std::getenv("FAST_BUFFER_EVICT");
    return !(value && value[0] == '0');
}();

void BufferCache::RunGarbageCollector(u64 transition_request) {
    staging_pool.TickFrame();
    SCOPE_EXIT {
        ++gc_tick;
    };
    PollBufferGrowSwitch();
    SettleGpuWrites();
    // VR_VRAM_REPORT: every 10 s, the cache's buffers (and their right-eye twins) by time since
    // last lookup or bind (last_used_tick). Under pressure EvictIdleBuffers frees idle ones.
    if (VramReport::Enabled()) {
        static std::array<u64, 64> tick_at{}; // gc_tick at each second, by second % 64
        const u64 second = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
        static u64 last_second = 0, reported_second = 0;
        if (second != last_second) {
            last_second = second;
            tick_at[second % tick_at.size()] = gc_tick;
        }
        if (second >= reported_second + 10 && gc_tick > 0) {
            reported_second = second;
            const u64 t10 = tick_at[(second - 10) % tick_at.size()];
            const u64 t60 = tick_at[(second - 60) % tick_at.size()];
            std::array<std::pair<u64, u64>, 3> bytes{}; // age: <10 s, 10-60 s, >60 s (own, twin)
            for (const auto& buffer : slot_buffers) {
                if (buffer.is_deleted || !buffer.CpuAddr())
                    continue;
                const u64 tick = std::atomic_ref<u64>(const_cast<u64&>(buffer.last_used_tick))
                                     .load(std::memory_order_relaxed);
                const u32 age = tick >= t10 ? 0 : (t60 && tick < t60) ? 2 : 1;
                bytes[age].first += buffer.SizeBytes();
                if (NativeStereo::ExistingRight(buffer.Handle()))
                    bytes[age].second += buffer.SizeBytes();
            }
            LOG_INFO(Render_Vulkan,
                     "VR VRAM report: buffer cache by last use: <10 s {} MiB (+{} right-eye), "
                     "10-60 s {} MiB (+{}), >60 s {} MiB (+{})",
                     bytes[0].first >> 20, bytes[0].second >> 20, bytes[1].first >> 20,
                     bytes[1].second >> 20, bytes[2].first >> 20, bytes[2].second >> 20);
            if (sparse_pool.Enabled()) {
                // The right-eye column above counts a sparse twin at its view's size; its memory
                // is only its own blocks (the rest shares the left pieces): those are here.
                LOG_INFO(Render_Vulkan,
                         "VR VRAM report: sparse views: {} MiB in pieces ({} MiB of them the right "
                         "eye's own), {} MiB of slabs",
                         sparse_pool.ResidentBytes() >> 20, sparse_pool.PrivateBytes() >> 20,
                         sparse_pool.SlabBytes() >> 20);
            }
            // The largest buffers idle over 10 s, with whether the GPU wrote them.
            std::vector<std::tuple<u64, VAddr, bool, bool>> idle; // size, address, twin, gpu
            for (const auto& buffer : slot_buffers) {
                if (buffer.is_deleted || !buffer.CpuAddr() || buffer.SizeBytes() < (4u << 20))
                    continue;
                const u64 tick = std::atomic_ref<u64>(const_cast<u64&>(buffer.last_used_tick))
                                     .load(std::memory_order_relaxed);
                if (tick < t10)
                    idle.emplace_back(buffer.SizeBytes(), buffer.CpuAddr(),
                                      bool(NativeStereo::ExistingRight(buffer.Handle())),
                                      gpu_modified_ranges.Intersects(buffer.CpuAddr(),
                                                                     buffer.SizeBytes()));
            }
            std::ranges::sort(idle, std::greater{});
            std::string list;
            for (size_t i = 0; i < std::min<size_t>(idle.size(), 12); ++i) {
                const auto& [size, address, twin, gpu] = idle[i];
                list += fmt::format(" {:#x}+{}MiB{}{}", address, size >> 20, twin ? " twin" : "",
                                    gpu ? " gpu" : "");
            }
            LOG_INFO(Render_Vulkan, "VR VRAM report: buffers idle over 10 s, largest:{}", list);
        }
    }
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    const u64 second = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
    gc_seconds.Record(second, gc_tick);
    if (sparse_pool.Enabled()) {
        // Slabs no view binds any more go back to the driver after a while (SparsePool).
        sparse_pool.Collect(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count()));
    }
    // A load: the marks follow Windows' budget for the game now (ReviseMemoryMarks), once per
    // request, as the texture cache's.
    if (const u64 sequence = transition_request >> 1; sequence != revised_sequence) {
        revised_sequence = sequence;
        ReviseMemoryMarks();
    } else if (live_budget.Due(second)) {
        ReviseMemoryMarks(true); // during play too (TextureMemory::LiveBudget)
    }
    // Trim before Windows needs to page. A typical first visit already uses about 600 MiB of
    // sparse backing. Once 640 MiB of sparse pieces are in use (twenty 32 MiB slabs' worth), a
    // reload can reclaim CPU-backed buffers left behind, even on a card whose overall memory use
    // is still below its pressure mark.
    constexpr u64 sparse_trim_start = 640_MB;
    const bool pressured = total_used_memory >= trigger_gc_memory;
    if (g_buffer_evict && !DmaTable::allocated.load(std::memory_order_relaxed)) {
        bool gentle = false;
        if (const auto cutoff = transition_cleanup.Step(second, transition_request, gc_tick, [&] {
                gentle = !pressured && sparse_pool.Enabled() &&
                         sparse_pool.ResidentBytes() >= sparse_trim_start;
                return pressured || gentle;
            })) {
            evict_second = second;
            if (gentle) {
                // A reload can still reuse the right eye's GPU-written data. Keep the ordinary
                // in-play eligibility rules and give recently bound resources a grace period.
                const u64 aged = gc_seconds.Before(second, 5);
                if (aged != 0) {
                    EvictUnusedSince(std::min(*cutoff, aged), BufferEviction::MaxPerCriticalPass,
                                     BufferEviction::MaxBytesPerPass, false);
                }
            } else {
                EvictUnusedSince(*cutoff, BufferEviction::MaxPerTransitionPass,
                                 BufferEviction::MaxBytesPerTransitionPass, true);
            }
        }
    }
    if (!pressured) {
        // While roaming, apply the existing one-minute idle rule once every five seconds. The
        // common frame does no scan. Small pools do no work beyond this occasional size check.
        if (sparse_pool.Enabled() && g_buffer_evict && second % 5 == 0 &&
            second != evict_second && !DmaTable::allocated.load(std::memory_order_relaxed)) {
            if (sparse_pool.ResidentBytes() >= sparse_trim_start) {
                EvictIdleBuffers(false, second);
            } else {
                evict_second = second;
            }
        }
        return;
    }
    // Over the critical mark the 5 s rule, never resting: a rest let memory climb over the budget
    // while travelling (TextureMemory::CriticalBackoff), and a buffer unused 5 s is rarely needed
    // the next moment, unlike a texture briefly off screen.
    EvictIdleBuffers(total_used_memory >= critical_gc_memory, second);
}


// FAST_BUFFER_EVICT_VERIFY=1 (a test): each evicted buffer's bytes against the game's, where the
// tracker calls the game's page unchanged since the upload (a page the CPU wrote since differs
// rightly). A difference there means the eviction loses bytes only the buffer had.
static const bool g_buffer_evict_verify = [] {
    const char* value = std::getenv("FAST_BUFFER_EVICT_VERIFY");
    return value && value[0] == '1';
}();

void BufferCache::VerifyEviction(Buffer& buffer) {
    const VAddr address = buffer.CpuAddr();
    const u64 bytes = buffer.SizeBytes();
    if (bytes > 64_MB) {
        return;
    }
    static constexpr u64 Page = 4_KB;
    std::vector<u8> guest(bytes), cpu_modified(Common::DivCeil(bytes, Page));
    memory->CopySparseMemory(address, guest.data(), bytes);
    for (u64 i = 0; i < cpu_modified.size(); ++i) {
        cpu_modified[i] = memory_tracker->IsRegionCpuModified(address + i * Page,
                                                              std::min(Page, bytes - i * Page));
    }
    auto readback = std::make_shared<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                             vk::BufferUsageFlagBits::eTransferDst, bytes);
    const vk::MemoryBarrier2 to_transfer{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    scheduler.EndRendering(); // verification only
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &to_transfer});
    cmdbuf.copyBuffer(buffer.Handle(), readback->Handle(), vk::BufferCopy{0, 0, bytes});
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &to_host});
    const u64 serial = buffer.perf_serial;
    scheduler.DeferOperation([readback, guest = std::move(guest),
                              cpu_modified = std::move(cpu_modified), address, bytes, serial] {
        static std::atomic<u64> checked{0}, bad{0};
        ++checked;
        u64 clean_bad = 0, dirty_bad = 0, first = ~u64{0};
        for (u64 i = 0; i < cpu_modified.size(); ++i) {
            const u64 begin = i * Page, size = std::min(Page, bytes - begin);
            if (std::memcmp(guest.data() + begin, readback->mapped_data.data() + begin, size) ==
                0) {
                continue;
            }
            if (cpu_modified[i]) {
                ++dirty_bad;
            } else {
                ++clean_bad;
                first = std::min(first, begin);
            }
        }
        if (clean_bad) {
            ++bad;
            LOG_WARNING(Render_Vulkan,
                        "Buffer evict verify: {:#x}+{:#x} (serial {}) differs on {} unchanged "
                        "pages (first +{:#x}), {} CPU-written; {} of {} evictions bad",
                        address, bytes, serial, clean_bad, first, dirty_bad, bad.load(),
                        checked.load());
        } else if ((checked & 63) == 0) {
            LOG_INFO(Render_Vulkan, "Buffer evict verify: {} evictions checked, {} bad",
                     checked.load(), bad.load());
        }
    });
}

void BufferCache::SetMemoryMarks(s64 budget) {
    const s64 min_spacing_expected = budget - 1_GB;
    const s64 mem_threshold = std::min<s64>(budget, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(std::max<u64>(
        std::min(budget - min_vacancy_expected, min_spacing_expected), DEFAULT_TRIGGER_GC_MEMORY));
    // Within a small margin of the budget, as the textures' (TextureMemory::CriticalMark).
    critical_gc_memory = static_cast<u64>(
        TextureMemory::CriticalMark(budget, static_cast<s64>(trigger_gc_memory)));
    marks_budget = budget;
}

void BufferCache::ReviseMemoryMarks(bool live) {
    if (start_budget <= 0) {
        return;
    }
    const auto now = instance.QueryMemoryBudget();
    auto revised = TextureMemory::RevisedBudget(marks_budget, start_budget,
                                                static_cast<s64>(now.budget),
                                                static_cast<s64>(instance.GetDeviceLocalCap()));
    if (live) {
        revised = live_budget.Confirm(revised);
        // Windows moving the game's memory to system memory counts, whatever its budget reads.
        // Once per new Windows reading. None (the counters unavailable or stale) or the same one
        // again: the watch keeps its state, holding the marks while active, counting nothing.
        auto spill = TextureMemory::SpillWatch::Answer{spill_watch.Active(), std::nullopt};
        if (now.residency_sample != 0 && now.residency_sample != spill_sample) {
            spill_sample = now.residency_sample;
            spill = spill_watch.Step(static_cast<s64>(now.usage), static_cast<s64>(now.spilled),
                                     marks_budget);
        }
        if (Common::Log::Enabled && ++spill_readings % 15 == 0) {
            LOG_INFO(Render_Vulkan, "{}: residency: Vulkan's VRAM use {} MiB, resident {} MiB, "
                     "system memory {} MiB (its own {} MiB): spilled {} MiB",
                     "Buffer cache", now.usage >> 20, now.resident >> 20, now.non_local >> 20,
                     now.host >> 20, now.spilled >> 20);
        }
        if (spill.hold) {
            revised = spill.budget;
            if (spill.budget) {
                LOG_WARNING(Render_Vulkan,
                            "{}: Windows keeps {} MiB of the game's {} MiB in VRAM (the rest is "
                            "in system memory) while its budget reads {} MiB: the marks drop to "
                            "{} MiB",
                            "Buffer cache", now.resident >> 20, now.usage >> 20, now.budget >> 20,
                            *spill.budget >> 20);
            }
        } else if (spill_was_active) {
            LOG_INFO(Render_Vulkan, "{}: the game is all in VRAM again: Windows' budget sets "
                     "the marks again", "Buffer cache");
        }
        spill_was_active = spill_watch.Active();
    }
    if (!revised) {
        return;
    }
    const s64 before = marks_budget;
    SetMemoryMarks(*revised);
    LOG_INFO(Render_Vulkan,
             "Buffer cache: {} Windows' budget for the game is {} MiB (its use {} MiB): "
             "the marks follow it, {} -> {} MiB (eviction under pressure from {} MiB, critical "
             "from {} MiB)",
             live ? "during play" : "at a load", now.budget >> 20, now.usage >> 20, before >> 20, *revised >> 20,
             trigger_gc_memory >> 20, critical_gc_memory >> 20);
}

void BufferCache::EvictIdleBuffers(bool critical, u64 second) {
    // DMA: shaders reach buffers through the address table, bound or not; no idle age tells.
    if (!g_buffer_evict || second == evict_second ||
        DmaTable::allocated.load(std::memory_order_relaxed)) {
        return;
    }
    evict_second = second;
    // Uses before the start of the second IdleSeconds back (0: not running that long yet).
    const u64 cutoff = gc_seconds.Before(second, BufferEviction::IdleSeconds(critical));
    if (cutoff == 0) {
        return;
    }
    EvictUnusedSince(cutoff, critical ? BufferEviction::MaxPerCriticalPass : BufferEviction::MaxPerPass,
                     critical ? BufferEviction::MaxBytesPerPass * 4 : BufferEviction::MaxBytesPerPass,
                     false);
}

void BufferCache::EvictUnusedSince(u64 cutoff, std::size_t max_count, u64 max_bytes,
                                   bool transition) {
    const u64 second = evict_second;
    std::vector<BufferEviction::Candidate> candidates;
    for (u32 index = 0; index < slot_buffers.capacity(); ++index) {
        const BufferId id{index};
        if (!slot_buffers.is_allocated(id)) {
            continue;
        }
        const Buffer* const buffer = &slot_buffers[id];
        if (buffer->is_deleted || !buffer->CpuAddr()) {
            continue;
        }
        const u64 tick = std::atomic_ref<u64>(const_cast<u64&>(buffer->last_used_tick))
                             .load(std::memory_order_relaxed);
        if (tick >= cutoff || buffer->image_data) {
            continue; // the cheap tests first; the GPU and twin tests for the idle ones only
        }
        const VAddr address = buffer->CpuAddr();
        const u64 bytes = buffer->SizeBytes();
        candidates.push_back({
            .id = index,
            .bytes = bytes,
            .last_used_tick = tick,
            .gpu_written = gpu_modified_ranges.Intersects(address, bytes) ||
                           memory_tracker->IsRegionGpuModified(address, bytes),
            .twin = bool(NativeStereo::ExistingRight(buffer->Handle())),
            .image_data = buffer->image_data,
        });
    }
    // Idle buffers that keep a right-eye twin: the twin alone goes, the biggest first within the
    // same bounds (a twin is half of a per-eye buffer's memory; the 2026-10-26 soak's twins grew
    // 72 -> 872 MiB). A GPU-written buffer's twin only after a transition (TwinReleasable).
    std::vector<BufferEviction::Candidate> twins;
    for (const auto& c : candidates) {
        if (BufferEviction::TwinReleasable(c, cutoff, transition)) {
            twins.push_back({.id = c.id, .bytes = c.bytes, .last_used_tick = c.last_used_tick});
        }
    }
    auto twin_victims = BufferEviction::Pick(std::move(twins), cutoff, max_count, max_bytes);
    // Each release makes the binding memos rebuild once (a new stereo buffer generation): an idle
    // pass frees twins only in a batch worth it; after a transition the load hides it.
    u64 twin_bytes = 0;
    for (const u32 index : twin_victims) {
        twin_bytes += slot_buffers[BufferId{index}].SizeBytes();
    }
    if (!transition && twin_bytes < BufferEviction::MinTwinBytesPerIdlePass) {
        twin_victims.clear();
    }
    for (const u32 index : twin_victims) {
        const u64 freed = NativeStereo::ReleaseRight(scheduler, slot_buffers[BufferId{index}].Handle());
        if (freed) {
            ++released_twins;
            released_twin_bytes += freed;
        }
    }
    const auto victims =
        BufferEviction::Pick(std::move(candidates), cutoff, max_count, max_bytes);
    for (const u32 index : victims) {
        const BufferId buffer_id{index};
        Buffer& buffer = slot_buffers[buffer_id];
        const VAddr address = buffer.CpuAddr();
        const u64 bytes = buffer.SizeBytes();
        if (g_buffer_evict_verify) {
            VerifyEviction(buffer);
        }
        // The game's memory holds every byte (no GPU write): a buffer made here later uploads
        // them (the tracker's pages CPU-modified), as for a page never cached.
        memory_tracker->MarkRegionAsCpuModified(address, bytes);
        memory_tracker->ForgetKept(address, bytes);
        DeleteBuffer(buffer_id);
        ++evicted_buffers;
        evicted_bytes += bytes;
    }
    if ((!victims.empty() || !twin_victims.empty()) &&
        (transition || second >= evict_logged_second + 10)) {
        evict_logged_second = second;
        LOG_INFO(Render_Vulkan,
                 "Buffer cache: evicted {} buffers ({} MiB) and {} right-eye twins ({} MiB) so "
                 "far{}; VRAM {} MiB of {} (pressure {} MiB)",
                 evicted_buffers, evicted_bytes >> 20, released_twins, released_twin_bytes >> 20,
                 transition ? ", this pass after a transition" : "", total_used_memory >> 20,
                 marks_budget >> 20, trigger_gc_memory >> 20);
    }
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);
}

// FAST_BACKGROUND_FREE ('1' on, off by default; read once at start-up. 2026-10-24: no hitch
// it removes measured yet, ~2-3% fps at the sewers spot): a deleted buffer's memory is freed
// on the scheduler's background thread (every cache buffer has a dedicated allocation: a ~1 ms
// vkFreeMemory each, which the sewers' streaming did hundreds of times in one frame, 2026-10-24).
// The slot and the native stereo entry are still released on the recording thread.
static const bool g_background_free = [] {
    const char* value = std::getenv("FAST_BACKGROUND_FREE");
    return value && value[0] == '1';
}();

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    Unregister(buffer_id);
    // A sparse view's pieces lose its reference once the GPU is done with it (SparsePool).
    const bool view = buffer.sparse_view;
    const VAddr view_begin = buffer.CpuAddr();
    scheduler.DeferOperation([this, buffer_id, view, view_begin,
                              view_pieces = std::move(buffer.sparse_pieces)] {
        if (g_background_free || FastRender::On(FastRender::BackgroundFree)) {
            const auto released = slot_buffers[buffer_id].buffer.Release();
            if (released.buffer) {
                scheduler.DeferPriorityOperation(
                    [released] { UniqueBuffer::DestroyReleased(released); });
            }
        }
        slot_buffers.erase(buffer_id);
        if (view) {
            sparse_pool.ReleaseView(view_begin, view_pieces);
        }
    });
    buffer.is_deleted = true;
}

} // namespace VideoCore
