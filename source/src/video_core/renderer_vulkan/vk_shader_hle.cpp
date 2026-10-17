// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

#include "shader_recompiler/info.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// FAST bit 57 (gpu_plan.md H1): all of a copy shader call's buffers are looked up first, then each
// (source, destination) buffer pair gets one copy per eye, with no barrier between the batches.
// Today each of the ~8 batches per call is a lookup, a copy per eye and a transfer barrier: ~21
// serialized commands, 11.4 us of GPU time per call at 54 calls per frame. bbport records a
// barrier between copies only on a tracked range hazard (vk_shader_hle.cpp:103-107,
// vk_runtime.cpp:114-162); here the whole call is checked once, and anything that could need one
// takes today's path:
//  - a lookup recorded a command or submitted (an upload, a JoinOverlap migration, a buffer
//    created or deleted with its Buffer::Fill): Scheduler::CommandUses, CurrentTick and
//    BufferCache::CopyMemoEpoch changed;
//  - a buffer is both a source and a destination of the call;
//  - two regions write overlapping destination bytes.
// Read once per call and nothing is kept across calls, so it may flip between any two dispatches.
// Off: exactly today's commands. FAST_HLE_COPY_GROUPS_VERIFY=N checks every Nth grouped call.
// Bits 47-56 belong to the w6 and other g1 ports (52 is w6_dlss_preset's DlssPreset).
static constexpr auto HleCopyGroups = static_cast<FastRender::Bit>(57);

namespace {

enum class GroupFallback : u32 { None, Recorded, Alias, Overlap };

// The 'FAST hle_copy_groups:' counters (verified/mismatches/bytes are bumped by deferred
// operations).
struct HleCopyGroupStats {
    std::atomic<u64> calls{}, bit_off{}, skipped{}, grouped{}, batches{}, copy_calls{};
    std::atomic<u64> recorded{}, alias{}, overlap{};
    std::atomic<u64> verified{}, mismatches{}, verify_skipped{}, stale{};
    std::atomic<u64> verify_with_before{}, verify_changed{};
};
HleCopyGroupStats g_group_stats;

u64 BumpCounter(std::atomic<u64>& counter, u64 n = 1) {
    return counter.fetch_add(n, std::memory_order_relaxed) + n;
}

void ReportGroups(u64 calls) {
    if ((calls & 8191) != 0) {
        return;
    }
    const auto& s = g_group_stats;
    const auto get = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    LOG_INFO(Render_Vulkan,
             "FAST hle_copy_groups: {} calls, bit off {}, empty or huge {}; grouped {} ({} "
             "batches -> {} copy calls); today's path: lookups recorded {}, source is a "
             "destination {}, overlapping destinations {}; verified {} mismatches {} skipped {} "
             "stale lookups {}, bytes the copies changed {} of {} with a before image",
             calls, get(s.bit_off), get(s.skipped), get(s.grouped), get(s.batches),
             get(s.copy_calls), get(s.recorded), get(s.alias), get(s.overlap), get(s.verified),
             get(s.mismatches), get(s.verify_skipped), get(s.stale), get(s.verify_changed),
             get(s.verify_with_before));
}

/// One batch of a call: consecutive regions while the source span and the destination span both
/// stay within the merge distance (offsets relative to the sharps' base addresses).
struct CopyBatch {
    u32 begin{};
    u32 end{};
    vk::DeviceSize src_min{};
    vk::DeviceSize src_max{};
    vk::DeviceSize dst_min{};
    vk::DeviceSize dst_max{};
};

CopyBatch NextBatch(std::span<const vk::BufferCopy> copies, u32 begin, vk::DeviceSize max_span) {
    // Place first copy into the current batch
    const auto& copy = copies[begin];
    CopyBatch batch{
        .begin = begin,
        .end = begin + 1,
        .src_min = copy.srcOffset,
        .src_max = copy.srcOffset + copy.size,
        .dst_min = copy.dstOffset,
        .dst_max = copy.dstOffset + copy.size,
    };
    for (; batch.end < copies.size(); batch.end++) {
        // Compute new src and dst bounds if we were to batch this copy
        const auto& [src_offset, dst_offset, size] = copies[batch.end];
        const auto new_src_min = std::min(batch.src_min, src_offset);
        const auto new_src_max = std::max(batch.src_max, src_offset + size);
        if (new_src_max - new_src_min > max_span) {
            break;
        }

        const auto new_dst_min = std::min(batch.dst_min, dst_offset);
        const auto new_dst_max = std::max(batch.dst_max, dst_offset + size);
        if (new_dst_max - new_dst_min > max_span) {
            break;
        }

        // We can batch this copy
        batch.src_min = new_src_min;
        batch.src_max = new_src_max;
        batch.dst_min = new_dst_min;
        batch.dst_max = new_dst_max;
    }
    return batch;
}

/// A batch with the buffers its lookup returned.
struct LookedUp {
    CopyBatch batch;
    vk::Buffer src;
    vk::Buffer dst;
    u32 src_offset{};
    u32 dst_offset{};
    bool grouped{}; // bit 57's phase 2: already in a group
};

/// Bit 57: one copy per eye from `src` to `dst` of the gathered regions [first, first + count).
struct CopyGroup {
    vk::Buffer src;
    vk::Buffer dst;
    u32 first{};
    u32 count{};
};

/// Bit 57: a buffer is a source and a destination of the call. A batch could then read what an
/// earlier one wrote, or write what an earlier one reads. Right-eye twins are never cached buffers,
/// so equal left handles are the only way the two eyes' copies can alias too.
bool SourceIsDestination(std::span<const LookedUp> looked_up) {
    static std::vector<vk::Buffer> dsts; // recording owner only, like the HLE's own vectors
    dsts.clear();
    for (const auto& l : looked_up) {
        if (std::find(dsts.begin(), dsts.end(), l.dst) == dsts.end()) {
            dsts.push_back(l.dst);
        }
    }
    for (const auto& l : looked_up) {
        if (std::find(dsts.begin(), dsts.end(), l.src) != dsts.end()) {
            return true;
        }
    }
    return false;
}

/// Bit 57: two regions of the call write overlapping destination bytes (as guest offsets: while
/// the lookups recorded nothing, a guest range lies in one cached buffer at one offset, and
/// distinct cached buffers never share guest pages, so guest overlap is buffer overlap).
/// Overlaps inside one batch count too; today's path keeps them as they are.
bool DestinationsOverlap(std::span<const vk::BufferCopy> copies) {
    static std::vector<std::pair<u64, u64>> spans; // recording owner only
    spans.clear();
    spans.reserve(copies.size());
    for (const auto& copy : copies) {
        if (copy.size != 0) {
            spans.emplace_back(copy.dstOffset, copy.dstOffset + copy.size);
        }
    }
    if (!std::is_sorted(spans.begin(), spans.end())) {
        std::sort(spans.begin(), spans.end());
    }
    u64 end = 0;
    for (const auto& [span_begin, span_end] : spans) {
        if (span_begin < end) {
            return true;
        }
        end = std::max(end, span_end);
    }
    return false;
}

/// FAST_HLE_COPY_GROUPS_VERIFY, every grouped call: each batch's buffers are still the ones the page
/// table holds at its guest addresses. Phase 2 copies with the handles phase 1 returned. A lookup
/// that replaced one of them unnoticed (no CommandUses, CurrentTick or CopyMemoEpoch change) would
/// leave copies into a buffer no longer bound there, which the byte check cannot see: it reads the
/// same handles back. Stream-buffer sources (small read-only ranges copied on the CPU) are skipped.
bool LookupsStillLive(VideoCore::BufferCache& cache, std::span<const LookedUp> looked_up,
                      VAddr src_base, VAddr dst_base) {
    const vk::Buffer stream = cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).Handle();
    const auto live = [&cache](VAddr addr, vk::Buffer handle, u32 offset) {
        const VideoCore::BufferId id = cache.BufferIdAt(addr);
        if (!id) {
            return false;
        }
        const VideoCore::Buffer& buffer = cache.GetBuffer(id);
        return buffer.Handle() == handle && buffer.Offset(addr) == offset;
    };
    for (const auto& l : looked_up) {
        if (!live(dst_base + l.batch.dst_min, l.dst, l.dst_offset) ||
            (l.src != stream && !live(src_base + l.batch.src_min, l.src, l.src_offset))) {
            return false;
        }
    }
    return true;
}

/// FAST_HLE_COPY_GROUPS_VERIFY=N: every Nth grouped call reads back each region's destination bytes
/// before its copies (both eyes where the right twin exists), and its source and destination bytes
/// after them, then compares them once the GPU is done. Today's path leaves exactly the source bytes
/// in every region (the destinations are disjoint and no buffer is both read and written), so the
/// grouped copies must too. A destination that already held the source bytes proves nothing (static
/// objects rewrite the same values every frame), so the counter line also gives how many of the
/// bytes with a before image the copies changed. One check in flight, at most 4 MB of regions; the
/// extra barriers and copies exist only in this mode.
class GroupReadback {
public:
    /// Before the grouped copies. False: skipped (nothing recorded).
    bool Begin(Rasterizer& rasterizer, std::span<const CopyGroup> groups,
               std::span<const vk::BufferCopy> regions);
    /// After them; only after a Begin that returned true.
    void Finish(Rasterizer& rasterizer, std::span<const CopyGroup> groups,
                std::span<const vk::BufferCopy> regions);

private:
    struct Span {
        u64 begin{};
        u64 end{};
        bool right_before{}; // the right twin existed before the copies (its before image read)
        bool right_after{};  // it exists after them
    };
    // Slots of `total` bytes each.
    enum Slot : u64 { LeftSrc, LeftDst, RightSrc, RightDst, LeftBefore, RightBefore, SlotCount };

    void Read(const RecorderProxy& cmdbuf, vk::Buffer from, std::span<const vk::BufferCopy> regions,
              bool destination, Slot slot, u64 at) const;

    static inline std::atomic<bool> in_flight{false};
    VmaAllocator allocator{};
    VkBuffer readback{};
    VmaAllocation allocation{};
    const void* mapped{};
    u64 total{};
    std::vector<Span> spans;
};

void GroupReadback::Read(const RecorderProxy& cmdbuf, vk::Buffer from,
                         std::span<const vk::BufferCopy> regions, bool destination, Slot slot,
                         u64 at) const {
    static std::vector<vk::BufferCopy> reads; // recording owner only
    reads.clear();
    for (const auto& region : regions) {
        if (region.size != 0) {
            const vk::DeviceSize offset = destination ? region.dstOffset : region.srcOffset;
            reads.push_back(vk::BufferCopy{offset, slot * total + at, region.size});
        }
        at += region.size;
    }
    if (!reads.empty()) {
        cmdbuf.copyBuffer(from, vk::Buffer{readback}, reads);
    }
}

bool GroupReadback::Begin(Rasterizer& rasterizer, std::span<const CopyGroup> groups,
                          std::span<const vk::BufferCopy> regions) {
    auto& stats = g_group_stats;
    total = 0;
    for (const auto& region : regions) {
        total += region.size;
    }
    if (total == 0 || total > 4_MB || in_flight.exchange(true, std::memory_order_acq_rel)) {
        BumpCounter(stats.verify_skipped);
        return false;
    }
    allocator = rasterizer.GetInstance().GetAllocator();
    const VkBufferCreateInfo readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = total * SlotCount,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo readback_alloc_ci{
        .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(allocator, &readback_ci, &readback_alloc_ci, &readback, &allocation,
                        &allocation_info) != VK_SUCCESS) {
        in_flight.store(false, std::memory_order_release);
        BumpCounter(stats.verify_skipped);
        return false;
    }
    if (!allocation_info.pMappedData) {
        vmaDestroyBuffer(allocator, readback, allocation);
        in_flight.store(false, std::memory_order_release);
        BumpCounter(stats.verify_skipped);
        return false;
    }
    mapped = allocation_info.pMappedData;
    spans.clear();
    spans.reserve(groups.size());
    // The call's pre-barrier already made every earlier write visible to transfer reads.
    const auto cmdbuf = rasterizer.GetScheduler().Commands();
    u64 at = 0;
    for (const auto& group : groups) {
        const auto group_regions = regions.subspan(group.first, group.count);
        const vk::Buffer right_dst = NativeStereo::ExistingRight(group.dst);
        Read(cmdbuf, group.dst, group_regions, true, LeftBefore, at);
        if (right_dst) {
            Read(cmdbuf, right_dst, group_regions, true, RightBefore, at);
        }
        u64 size = 0;
        for (const auto& region : group_regions) {
            size += region.size;
        }
        spans.push_back(Span{.begin = at, .end = at + size, .right_before = bool(right_dst)});
        at += size;
    }
    // The copies overwrite what was just read: they wait for those reads.
    const vk::MemoryBarrier2 before_copies{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &before_copies});
    return true;
}

void GroupReadback::Finish(Rasterizer& rasterizer, std::span<const CopyGroup> groups,
                           std::span<const vk::BufferCopy> regions) {
    const auto cmdbuf = rasterizer.GetScheduler().Commands();
    const vk::MemoryBarrier2 to_readback{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &to_readback});
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        auto& span = spans[i];
        const auto group_regions = regions.subspan(group.first, group.count);
        const vk::Buffer right_src = NativeStereo::ExistingRight(group.src);
        const vk::Buffer right_dst = NativeStereo::ExistingRight(group.dst);
        Read(cmdbuf, group.src, group_regions, false, LeftSrc, span.begin);
        Read(cmdbuf, group.dst, group_regions, true, LeftDst, span.begin);
        if (right_dst) {
            // NativeStereo::Copy: the right destination takes the right source, else the left. A
            // twin made by a later group starts as a copy of the left destination.
            Read(cmdbuf, right_src ? right_src : group.src, group_regions, false, RightSrc,
                 span.begin);
            Read(cmdbuf, right_dst, group_regions, true, RightDst, span.begin);
        }
        span.right_after = static_cast<bool>(right_dst);
    }
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &to_host});
    rasterizer.GetScheduler().DeferOperation([allocator = allocator, readback = readback,
                                              allocation = allocation, mapped = mapped,
                                              total = total, spans = std::move(spans)]() {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        const auto* const bytes = static_cast<const u8*>(mapped);
        const auto at = [&](Slot slot, const Span& part) {
            return bytes + slot * total + part.begin;
        };
        auto& stats = g_group_stats;
        bool equal = true;
        u64 with_before = 0;
        u64 changed = 0;
        for (size_t i = 0; i < spans.size(); ++i) {
            const auto& part = spans[i];
            const u64 size = part.end - part.begin;
            for (u32 eye = 0; eye < (part.right_after ? 2u : 1u); ++eye) {
                const u8* const want = at(eye == 0 ? LeftSrc : RightSrc, part);
                const u8* const have = at(eye == 0 ? LeftDst : RightDst, part);
                if (eye == 0 || part.right_before) {
                    const u8* const before = at(eye == 0 ? LeftBefore : RightBefore, part);
                    with_before += size;
                    for (u64 b = 0; b < size; ++b) {
                        changed += before[b] != want[b] ? 1 : 0;
                    }
                }
                if (std::memcmp(want, have, size) == 0) {
                    continue;
                }
                if (equal) { // one count per call
                    const auto differs = std::mismatch(want, want + size, have);
                    const u64 n = BumpCounter(stats.mismatches);
                    if (n <= 16) {
                        LOG_ERROR(Render_Vulkan,
                                  "FAST hle_copy_groups verify: mismatch #{}: group {} of {}, "
                                  "{} eye, {} bytes, first at +{} (source {:#x}, destination "
                                  "{:#x})",
                                  n, i, spans.size(), eye == 0 ? "left" : "right", size,
                                  differs.first - want, u32{*differs.first},
                                  u32{*differs.second});
                    }
                }
                equal = false;
            }
        }
        BumpCounter(stats.verify_with_before, with_before);
        BumpCounter(stats.verify_changed, changed);
        BumpCounter(stats.verified);
        vmaDestroyBuffer(allocator, readback, allocation);
        in_flight.store(false, std::memory_order_release);
    });
}

} // namespace

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& scheduler = rasterizer.GetScheduler();
    auto& buffer_cache = rasterizer.GetBufferCache();
    // VR_PERF_CHECK / VR_PERF_STATS: what a range cache could skip here (perf2 plan, point 2c).
    const bool stats = Rasterizer::PerfStats();
    const auto stats_t0 = stats ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    if (stats) {
        auto& cs = rasterizer.copy_stats;
        ++cs.calls;
        cs.copies += copies.size();
        // The call's signature: the control data, both sharps' addresses, the count.
        u64 h = 0x9E3779B97F4A7C15ull ^ cs_program.dim_x;
        const auto mix = [&h](u64 v) {
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 31;
        };
        mix(src_buf_sharp.base_address);
        mix(dst_buf_sharp.base_address);
        mix(buf_stride);
        for (const auto& c : copies) {
            mix(c.srcOffset);
            mix(c.dstOffset);
            mix(c.size);
        }
        bool seen = false;
        for (const u64 r : cs.recent) {
            seen = seen || r == h;
        }
        cs.repeats += seen ? 1 : 0;
        cs.recent[cs.recent_next] = h;
        cs.recent_next = (cs.recent_next + 1) % cs.recent.size();
    }

    scheduler.EndRendering();
    // FAST_GPU_PASS_PROFILE (bit 38): one label for the HLE'd dispatches (bbport
    // vk_shader_hle.cpp:99-101); consecutive ones share a segment, uploads mark themselves.
    GpuPassProfiler::MarkAt(scheduler, 0xC0B1'0000'0000ull ^ COPY_SHADER_HASH,
                            GpuPassProfiler::Group::HleCopy,
                            "copy shader fefebf9f HLE (barriers, buffer lookups, copies)");

    static constexpr vk::MemoryBarrier READ_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    static constexpr vk::MemoryBarrier WRITE_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    scheduler.Commands().pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});

    // bbport's copy-span experiment: preserve every copy and its order while
    // avoiding synchronization of very large holes between small copied ranges.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* value = std::getenv("VR_COPY_MERGE_KB");
        if (!value) return vk::DeviceSize(16 * 1024);
        char* end = nullptr;
        const auto kb = std::strtoul(value, &end, 10);
        if (*end || (kb != 16 && kb != 64 && kb != 256 && kb != 65536))
            return vk::DeviceSize(16 * 1024);
        LOG_INFO(Render_Vulkan, "Scratch copy merge span: {} KiB", kb);
        return vk::DeviceSize(kb) * 1024;
    }();
    static const bool hle_outer_barriers = [] {
        const char* setting = std::getenv("VR_NATIVE_HLE_OUTER_BARRIERS");
        return !setting || setting[0] != '0';
    }();
    // VR_PERF_STATS: one batch's ranges and their CPU/GPU state.
    const auto note_batch = [&](const CopyBatch& batch) {
        auto& cs = rasterizer.copy_stats;
        const VAddr src_addr = src_buf_sharp.base_address + batch.src_min;
        const u64 src_size = batch.src_max - batch.src_min;
        const VAddr dst_addr = dst_buf_sharp.base_address + batch.dst_min;
        const u64 dst_size = batch.dst_max - batch.dst_min;
        ++cs.batches;
        cs.src_bytes += src_size;
        cs.dst_bytes += dst_size;
        cs.src_cpu_mod += buffer_cache.IsRegionCpuModified(src_addr, src_size) ? 1 : 0;
        cs.src_gpu_mod += buffer_cache.IsRegionGpuModified(src_addr, src_size) ? 1 : 0;
        cs.dst_cpu_mod += buffer_cache.IsRegionCpuModified(dst_addr, dst_size) ? 1 : 0;
        cs.dst_gpu_mod += buffer_cache.IsRegionGpuModified(dst_addr, dst_size) ? 1 : 0;
    };
    // Obtain buffers for the total source and destination ranges (perf_copy_memo: the
    // buffers remembered for the same ranges while no page there changed state).
    const auto look_up = [&](const CopyBatch& batch) {
        const auto [src_buf, src_buf_offset, dst_buf, dst_buf_offset] =
            rasterizer.ObtainCopyBuffers(src_buf_sharp.base_address + batch.src_min,
                                         static_cast<u32>(batch.src_max - batch.src_min),
                                         dst_buf_sharp.base_address + batch.dst_min,
                                         static_cast<u32>(batch.dst_max - batch.dst_min));
        return LookedUp{.batch = batch,
                        .src = src_buf->Handle(),
                        .dst = dst_buf->Handle(),
                        .src_offset = src_buf_offset,
                        .dst_offset = dst_buf_offset};
    };
    // Apply found buffer base.
    const auto rebase = [&](const LookedUp& l) {
        const auto vk_copies =
            std::span{copies}.subspan(l.batch.begin, l.batch.end - l.batch.begin);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - l.batch.src_min + l.src_offset;
            copy.dstOffset = copy.dstOffset - l.batch.dst_min + l.dst_offset;
        }
        return vk_copies;
    };
    // Today's path: each batch's lookup right before its copies. `known`: the lookups bit 57's
    // first phase made, which recorded nothing; they are used instead of being made again, so the
    // commands are exactly the ones looking up here would give.
    const auto record_interleaved = [&](std::span<const LookedUp> known, bool count_stats) {
        u32 index = 0;
        for (u32 begin = 0; begin < copies.size(); ++index) {
            // Smaller batches can revisit a backing buffer written by the previous batch,
            // including buffer-cache migration copies. Order these before the next lookup.
            if (begin != 0 && (MaxDistanceForMerge != 64_MB || hle_outer_barriers)) {
                scheduler.Commands().pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                    vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});
            }
            LookedUp batch;
            if (!known.empty()) {
                batch = known[index];
            } else {
                const CopyBatch next = NextBatch(copies, begin, MaxDistanceForMerge);
                if (count_stats) {
                    note_batch(next);
                }
                batch = look_up(next);
            }
            const auto vk_copies = rebase(batch);

            // Execute buffer copies.
            LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                      batch.batch.src_max - batch.batch.src_min,
                      batch.batch.dst_max - batch.batch.dst_min);
            NativeStereo::Copy(scheduler, batch.src, batch.dst, vk_copies, true);
            begin = batch.batch.end;
        }
    };

    auto& group_stats = g_group_stats;
    const u64 group_calls = BumpCounter(group_stats.calls);
    if (!FastRender::On(HleCopyGroups)) {
        BumpCounter(group_stats.bit_off);
        record_interleaved({}, stats);
    } else if (copies.empty() || copies.size() > 64 * 1024) {
        // Nothing to group, or one recorder packet per eye would grow past a few MB.
        BumpCounter(group_stats.skipped);
        record_interleaved({}, stats);
    } else {
        // Bit 57, phase 1: every batch's lookup, after the pre-barrier above and before any copy.
        // Anything a lookup records then follows the work the pre-barrier waits for and precedes
        // every copy of this call; whether it recorded anything is checked right after.
        static std::vector<LookedUp> looked_up;
        looked_up.clear();
        const u64 command_uses = scheduler.CommandUses();
        const u64 tick = scheduler.CurrentTick();
        const u64 epoch = buffer_cache.CopyMemoEpoch();
        for (u32 begin = 0; begin < copies.size();) {
            const CopyBatch batch = NextBatch(copies, begin, MaxDistanceForMerge);
            if (stats) {
                note_batch(batch);
            }
            looked_up.push_back(look_up(batch));
            begin = batch.end;
        }
        GroupFallback why = GroupFallback::None;
        if (scheduler.CommandUses() != command_uses || scheduler.CurrentTick() != tick ||
            buffer_cache.CopyMemoEpoch() != epoch) {
            why = GroupFallback::Recorded;
        } else if (SourceIsDestination(looked_up)) {
            why = GroupFallback::Alias;
        } else if (DestinationsOverlap(copies)) {
            why = GroupFallback::Overlap;
        }

        switch (why) {
        case GroupFallback::Recorded:
            // A lookup recorded work (an upload, a migration, a buffer created or deleted) or
            // submitted, so a buffer an earlier batch returned may be gone. Order that work before
            // every copy, then take today's path with lookups of its own (nothing left to record
            // there in the usual case).
            BumpCounter(group_stats.recorded);
            scheduler.Commands().pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});
            record_interleaved({}, false);
            break;
        case GroupFallback::Alias:
        case GroupFallback::Overlap:
            // Copies that may depend on each other: today's commands, from the lookups made.
            BumpCounter(why == GroupFallback::Alias ? group_stats.alias : group_stats.overlap);
            record_interleaved(looked_up, false);
            break;
        case GroupFallback::None: {
            // Phase 2: every region rebased into its batch's buffers, then one copy per eye for
            // each (source, destination) buffer pair, pairs in order of first use, regions in
            // their order (NativeStereo::Copy merges adjacent ones). Nothing in between needs a
            // barrier: the lookups recorded nothing, no buffer is both read and written, and no
            // two regions write the same bytes. The barriers before and after the call stay.
            static std::vector<vk::BufferCopy> gathered;
            static std::vector<CopyGroup> groups;
            gathered.clear();
            groups.clear();
            for (const auto& l : looked_up) {
                rebase(l);
            }
            for (size_t first = 0; first < looked_up.size(); ++first) {
                if (looked_up[first].grouped) {
                    continue;
                }
                const vk::Buffer src = looked_up[first].src;
                const vk::Buffer dst = looked_up[first].dst;
                const auto start = static_cast<u32>(gathered.size());
                for (size_t i = first; i < looked_up.size(); ++i) {
                    auto& l = looked_up[i];
                    if (l.grouped || l.src != src || l.dst != dst) {
                        continue;
                    }
                    l.grouped = true;
                    gathered.insert(gathered.end(), copies.begin() + l.batch.begin,
                                    copies.begin() + l.batch.end);
                }
                groups.push_back(CopyGroup{.src = src,
                                           .dst = dst,
                                           .first = start,
                                           .count = static_cast<u32>(gathered.size()) - start});
            }
            const u64 grouped = BumpCounter(group_stats.grouped);
            BumpCounter(group_stats.batches, looked_up.size());
            BumpCounter(group_stats.copy_calls, groups.size());
            static const u64 verify_period = [] {
                const char* value = std::getenv("FAST_HLE_COPY_GROUPS_VERIFY");
                const int period = value ? std::atoi(value) : 0;
                return period > 0 ? static_cast<u64>(period) : u64{0};
            }();
            if (verify_period != 0 &&
                !LookupsStillLive(buffer_cache, looked_up, src_buf_sharp.base_address,
                                  dst_buf_sharp.base_address)) {
                const u64 n = BumpCounter(group_stats.stale);
                if (n <= 16) {
                    LOG_ERROR(Render_Vulkan,
                              "FAST hle_copy_groups verify: stale lookup #{}: a batch's buffer is "
                              "no longer the one at its guest address ({} batches)",
                              n, looked_up.size());
                }
            }
            GroupReadback readback;
            const bool check = verify_period != 0 && grouped % verify_period == 0 &&
                               readback.Begin(rasterizer, groups, gathered);
            for (const auto& group : groups) {
                NativeStereo::Copy(scheduler, group.src, group.dst,
                                   std::span{gathered}.subspan(group.first, group.count), true);
            }
            if (check) {
                readback.Finish(rasterizer, groups, gathered);
            }
            break;
        }
        }
    }
    ReportGroups(group_calls);

    scheduler.Commands().pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlagBits::eByRegion, WRITE_BARRIER, {}, {});

    if (stats) {
        rasterizer.copy_stats.us += std::chrono::duration<double, std::micro>(
                                        std::chrono::steady_clock::now() - stats_t0)
                                        .count();
    }
    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
