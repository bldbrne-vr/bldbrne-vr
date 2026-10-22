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

namespace VideoCore {

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;

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
    u64 ranges, hits, small, misses, verified, races, mismatches;
};
VertexMemoStats g_vertex_memo_stats{};

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
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BDA_PAGETABLE_SIZE} {
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

    // The stream buffer's shadow (VR_STREAM_MEM, buffer.cpp) goes into the GPU's memory before
    // every submit of this scheduler.
    if (stream_buffer.HasShadow()) {
        scheduler.SetPreSubmitHook([this] { stream_buffer.FlushShadow(); });
    }

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Set up garbage collection parameters
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_TRIGGER_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
}

BufferCache::~BufferCache() {
    FastRender::WaitHostCopies();
    if (stream_buffer.HasShadow()) {
        scheduler.SetPreSubmitHook(Common::UniqueFunction<void>{});
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

std::span<const u8> BufferCache::ReadForMotion(VAddr address, u32 size) {
    FastRender::AssertRecordingAccess();
    if (!size || size > 16_MB) return {};
    if (!HasGpuWrites(address, size)) {
        return {reinterpret_cast<const u8*>(address), size};
    }
    Buffer& buffer = slot_buffers[FindBuffer(address, size)];
    // This is the same upload the upcoming indexed draw needs. It retires only
    // CPU-owned bytes; it never downloads old GPU contents into guest memory.
    SynchronizeBuffer(buffer, address, size, false, false, true);
    if (!HasGpuWrites(address, size)) {
        Common::MemoryWriteTrace::Event("motion.index.cpu_reclaimed", address, size);
        return {reinterpret_cast<const u8*>(address), size};
    }
    const u64 use = ++motion_snapshot_use;
    for (auto& entry : motion_snapshots) {
        if (entry.address == address && entry.bytes.size() == size &&
            entry.serial == buffer.perf_serial && entry.write_seq == buffer.perf_write_seq) {
            entry.last_use = use;
            return entry.bytes;
        }
    }
    // A stale version of this exact request cannot be reused. Remove it before
    // allocating its replacement so repeated writes cannot consume all cache slots.
    std::erase_if(motion_snapshots, [&](const MotionSnapshot& entry) {
        if (entry.address != address || entry.bytes.size() != size) return false;
        motion_snapshot_bytes -= entry.bytes.size();
        return true;
    });
    // Bound both allocations and entry count. A snapshot is valid only until the next
    // ReadForMotion call; the temporal caller consumes it during this same draw.
    while (!motion_snapshots.empty() &&
           (motion_snapshots.size() >= 16 || motion_snapshot_bytes + size > 16_MB)) {
        const auto oldest = std::min_element(motion_snapshots.begin(), motion_snapshots.end(),
            [](const auto& a, const auto& b) { return a.last_use < b.last_use; });
        motion_snapshot_bytes -= oldest->bytes.size();
        motion_snapshots.erase(oldest);
    }
    // Vulkan buffer copies are dword-aligned, including 16-bit indices starting at +2.
    const u64 begin = Common::AlignDown(static_cast<u64>(buffer.Offset(address)), u64{4});
    const u64 prefix = buffer.Offset(address) - begin;
    const u64 transfer_size = Common::AlignUp(prefix + size, u64{4});
    ASSERT(begin + transfer_size <= buffer.SizeBytes());
    const auto [download, staging_offset] = download_buffer.Map(transfer_size);
    download_buffer.Commit();
    scheduler.EndRendering();
    const auto commands = scheduler.Commands();
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer.Handle(), .offset = begin, .size = transfer_size,
    };
    commands.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
    commands.copyBuffer(buffer.Handle(), download_buffer.Handle(),
                        vk::BufferCopy{begin, staging_offset, transfer_size});
    // Completion alone does not replace the device-to-host visibility dependency.
    const vk::BufferMemoryBarrier2 host_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .buffer = download_buffer.Handle(), .offset = staging_offset, .size = transfer_size,
    };
    commands.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_barrier});
    scheduler.Finish();
    if (!download_buffer.is_coherent) {
        vmaInvalidateAllocation(instance.GetAllocator(), download_buffer.buffer.allocation,
                                staging_offset, transfer_size);
    }
    MotionSnapshot entry{address, buffer.perf_serial, buffer.perf_write_seq, use,
                         std::vector<u8>(size)};
    std::memcpy(entry.bytes.data(), download + prefix, size);
    motion_snapshot_bytes += size;
    motion_snapshots.push_back(std::move(entry));
    Common::MemoryWriteTrace::Event("motion.index.private_snapshot", address, size, transfer_size);
    return motion_snapshots.back().bytes;
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
    u64* motion_geometry, const Vulkan::VertexView* prepared) {
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
    if (instance.IsVertexInputDynamicState())
        scheduler.Commands().setVertexInputEXT(inputs.bindings, inputs.attributes);
    if (inputs.bindings.empty()) return;
    struct BufferRange {
        VAddr base_address, end_address;
        vk::Buffer vk_buffer;
        u64 offset;
        size_t GetSize() const { return end_address - base_address; }
    };
    Vulkan::VertexInputs<BufferRange> ranges_merged;
    for (const auto& range : inputs.ranges)
        ranges_merged.push_back({range.base_address, range.end_address, {}, 0});

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

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        const bool memo_range =
            vertex_memo && (size > CACHING_PAGESIZE || g_tracked_small_buffers);
        const BufferId clean_id =
            memo_range ? clean_buffer(range.base_address, static_cast<u32>(size)) : BufferId{};
        Buffer* const clean = clean_id ? &slot_buffers[clean_id] : nullptr;
        if constexpr (Common::Log::Enabled) {
            if (vertex_memo) {
                ++memo_stats.ranges;
                memo_stats.small += !memo_range;
                memo_stats.misses += memo_range && !clean;
                if ((memo_stats.ranges & 0xFFFF) == 0) {
                    LOG_INFO(Render_Vulkan,
                             "FAST_CPU_VERTEX_MEMO: ranges={} hits={} small={} misses={} "
                             "verified={} races={} mismatches={}",
                             memo_stats.ranges, memo_stats.hits, memo_stats.small,
                             memo_stats.misses, memo_stats.verified, memo_stats.races,
                             memo_stats.mismatches);
                }
            }
        }
        Buffer* buffer = clean;
        bool queried = false; // a cache buffer: the GPU-modified query below is the old path's
        if (clean && !g_vertex_memo_verify) {
            if constexpr (Common::Log::Enabled) {
                ++memo_stats.hits;
            }
            range.vk_buffer = clean->buffer;
            range.offset = static_cast<u32>(clean->Offset(range.base_address));
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
        if (!(queried && FastRender::On(UploadEarlyExit)) &&
            IsRegionGpuModified(range.base_address, size)) {
            if (auto barrier =
                    buffer->GetBarrier(vk::AccessFlagBits2::eVertexAttributeRead,
                                       vk::PipelineStageFlagBits2::eVertexAttributeInput)) {
                barriers.emplace_back(*barrier);
            }
        }
    }

    // Bind vertex buffers
    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
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
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.Commands();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void BufferCache::BindIndexBuffer(
    u32 index_offset, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers) {
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
            scheduler.Commands().bindIndexBuffer(bound->first->Handle(), bound->second,
                                                 index_type);
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
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindIndexBuffer(vk_buffer->Handle(), offset, index_type);
}

std::optional<std::pair<Buffer*, u32>> BufferCache::ObtainIndexBuffer(
    VAddr address, u32 size,
    boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers) {
    FastRender::AssertRecordingAccess();
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
        gpu_modified_ranges.Add(dst, num_bytes);
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

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                  bool is_texel_buffer, BufferId buffer_id,
                                                  SmallRead* small_read) {
    FastRender::AssertRecordingAccess();
    ScratchProfile::Cpu scratch_obtain(17,"BufferCache::ObtainBuffer");
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
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
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

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (IsRegionGpuModified(gpu_addr, size)) {
        return ObtainBuffer(gpu_addr, size, false, false);
    }
    // In all other cases, just do a CPU copy to the staging buffer.
    const auto [data, offset] = staging_buffer.Map(size, instance.StorageMinAlignment());
    memory->CopySparseMemory(gpu_addr, data, size);
    staging_buffer.Commit();
    return {&staging_buffer, offset};
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
        return CreateBuffer(device_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        return buffer_id;
    }
    return CreateBuffer(device_addr, size);
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

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
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
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            // Experiment (2026-10-18): grow by the region's own size (at most 128 MiB) instead of
            // 2 MiB. Streaming in an area re-created one region in 2 MiB steps: 2.7 GB allocated
            // in one frame for 589 MB asked, a 100 ms frame (the sewers).
            const VAddr leap = BufferGrowGeometric()
                                   ? std::clamp<VAddr>(end - begin, CACHING_PAGESIZE * 128, 128_MB)
                                   : CACHING_PAGESIZE * 128;
            if (expands_right) {
                expand_end(leap);
            }
            if (expands_left) {
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

void BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
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
}

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    Common::Slow::Scope slow{Common::Slow::CreateBuffer, wanted_size};
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin,
                            AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
    return new_buffer_id;
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
        boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
        bda_addrs.reserve(size_pages);
        for (u64 i = 0; i < size_pages; ++i) {
            vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
            bda_addrs.push_back(addr);
        }
        WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                        bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        std::unique_lock lk{buffer_ranges_mutex};
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        total_used_memory -= Common::AlignUp(size, CACHING_PAGESIZE);
        lru_cache.Free(buffer.LRUId());
        const u64 offset = bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
        bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
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
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    vk::Buffer src_buffer = VK_NULL_HANDLE;
    const bool async_upload = FastRender::On(FastRender::CopyThreads) && !is_written &&
                              staging_buffer.is_coherent && !staging_buffer.HasShadow();
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
        [&](bool safe_async) { src_buffer = UploadCopies(buffer, copies, total_size_bytes, safe_async); },
        async_upload);

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
        if (retire_cpu_uploads) {
            for (const auto& copy : copies) {
                gpu_modified_ranges.Subtract(buffer_start + copy.dstOffset, copy.size);
            }
            copy_memo_epoch.fetch_add(1, std::memory_order_acq_rel);
        }
        TouchBuffer(buffer);
        buffer.PerfNoteWrite(); // the resync memo: guest data uploaded into it
    }
    if (is_written) {
        // Bound for writing (a shader, a copy, a fill): written from here (a shader's writes come
        // with its draw: no memo is kept for this buffer during this draw's bindings).
        buffer.PerfNoteWriteBound();
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeBufferFromImage(buffer, device_addr, size);
    }
    return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     size_t total_size_bytes, bool async_upload) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    // Never wait on recorder/GPU progress while holding async source-region locks.
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes, 0, !async_upload);
    if (staging) {
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            if (async_upload)
                memory->QueueSparseMemory(device_addr, src_pointer, copy.size);
            else
                memory->CopySparseMemory(device_addr, src_pointer, copy.size);
            // Apply the staging offset
            copy.srcOffset += offset;
        }
        staging_buffer.Commit();
        return staging_buffer.Handle();
    } else {
        // For large one time transfers use a temporary host buffer.
        auto temp_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0,
                                     vk::BufferUsageFlagBits::eTransferSrc, total_size_bytes);
        const vk::Buffer src_buffer = temp_buffer->Handle();
        u8* const staging = temp_buffer->mapped_data.data();
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            memory->CopySparseMemory(device_addr, src_pointer, copy.size);
        }
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable { buffer.reset(); });
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    ScratchProfile::Cpu scratch_cpu(4,"BufferCache::SynchronizeBufferFromImage");
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            buffer.image_data = true; // bit 33: no longer guest bytes there
            buffer.Fill(buffer.Offset(device_addr), size, ZmaskUncompressed);
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
    vk::Buffer src_buffer = staging_buffer.Handle();
    if (num_bytes < StagingBufferSize) {
        const auto [staging, offset] = staging_buffer.Map(num_bytes);
        std::memcpy(staging, value, num_bytes);
        copy.srcOffset = offset;
        staging_buffer.Commit();
    } else {
        // For large one time transfers use a temporary host buffer.
        // RenderDoc can lag quite a bit if the stream buffer is too large.
        Buffer temp_buffer{
            instance, scheduler, MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc,
            num_bytes};
        src_buffer = temp_buffer.Handle();
        u8* const staging = temp_buffer.mapped_data.data();
        std::memcpy(staging, value, num_bytes);
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable {});
    }
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

void BufferCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    PollBufferGrowSwitch();
    SettleGpuWrites();
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    const bool aggressive = total_used_memory >= critical_gc_memory;
    const u64 ticks_to_destroy = std::min<u64>(aggressive ? 80 : 160, gc_tick);
    int max_deletions = aggressive ? 64 : 32;
    const auto clean_up = [&](BufferId buffer_id) {
        if (max_deletions == 0) {
            return;
        }
        --max_deletions;
        Buffer& buffer = slot_buffers[buffer_id];
        // The catch-all log (off for performance):
        // if (gpu_modified_ranges.Intersects(buffer.CpuAddr(), buffer.SizeBytes())) {
        //     static std::atomic<u64> logs{0};
        //     const u64 n = logs.fetch_add(1, std::memory_order_relaxed);
        //     if (n < 64 || (n & 255) == 0) {
        //         LOG_INFO(Render_Vulkan,
        //                  "FAST_GPU_PAGE_KEEP: GC evicts a buffer with GPU-written bytes at "
        //                  "{:#x}..{:#x} (downloaded, written back later; {} so far)",
        //                  buffer.CpuAddr(), buffer.CpuAddr() + buffer.SizeBytes(), n + 1);
        //     }
        // }
        // InvalidateMemory(buffer.CpuAddr(), buffer.SizeBytes());
        DownloadBufferMemory<true>(buffer, buffer.CpuAddr(), buffer.SizeBytes());
        memory_tracker->MarkRegionAsCpuModified(buffer.CpuAddr(), buffer.SizeBytes());
        // Its GPU copy goes: a buffer made there later uploads whole (FAST_GPU_PAGE_KEEP).
        memory_tracker->ForgetKept(buffer.CpuAddr(), buffer.SizeBytes());
        DeleteBuffer(buffer_id);
    };
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);
}

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    Unregister(buffer_id);
    scheduler.DeferOperation([this, buffer_id] { slot_buffers.erase(buffer_id); });
    buffer.is_deleted = true;
}

} // namespace VideoCore
