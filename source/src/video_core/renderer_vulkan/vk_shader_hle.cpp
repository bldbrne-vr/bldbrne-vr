// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

#include "common/rdtsc.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/fast_render.h"
#include "video_core/host_shaders/vr_hle_multi_copy_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

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
    std::atomic<u64> multi_dispatched{}, multi_fallback{}; // FAST_HLE_MULTI_COPY
};
HleCopyGroupStats g_group_stats;

u64 BumpCounter(std::atomic<u64>& counter, u64 n = 1) {
    return counter.fetch_add(n, std::memory_order_relaxed) + n;
}

// Reporting-only counters need no atomic traffic when logging is compiled out.
// Keep BumpCounter for verification cadence and mismatch/stale diagnostic limits.
u64 BumpLogCounter(std::atomic<u64>& counter, u64 n = 1) {
    if constexpr (Common::Log::Enabled) {
        return BumpCounter(counter, n);
    }
    return 0;
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
             "stale lookups {}, bytes the copies changed {} of {} with a before image; multi copy "
             "dispatched {} fallback {}",
             calls, get(s.bit_off), get(s.skipped), get(s.grouped), get(s.batches),
             get(s.copy_calls), get(s.recorded), get(s.alias), get(s.overlap), get(s.verified),
             get(s.mismatches), get(s.verify_skipped), get(s.stale), get(s.verify_changed),
             get(s.verify_with_before), get(s.multi_dispatched), get(s.multi_fallback));
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
bool DestinationsOverlapSorted(std::span<const vk::BufferCopy> copies) {
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

/// VR_HLE_OVERLAP (on by default; '0' the pair sort above; 'v' both, every call, answers
/// compared; 'a' both on alternate calls, timed, logged every 65536 calls): DestinationsOverlapSorted's
/// answer from one 64-bit key a region (offset << 32 | size: the regions are still relative to
/// the sharps' bases here, so both fit 32 bits) instead of a pair of them. A region outside 32 bits
/// takes the pair sort. Old Yharnam, alternate calls: ~9,500 -> ~7,900 cycles a call.
char OverlapMode() {
    static const char mode = [] {
        const char* v = std::getenv("VR_HLE_OVERLAP");
        return v && (v[0] == '0' || v[0] == 'v' || v[0] == 'a' || v[0] == 'b') ? v[0] : '1';
    }();
    return mode;
}

bool DestinationsOverlapPacked(std::span<const vk::BufferCopy> copies) {
    static std::vector<u64> keys; // recording owner only
    keys.clear();
    keys.reserve(copies.size());
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        if ((copy.dstOffset | copy.size) >> 32) {
            return DestinationsOverlapSorted(copies);
        }
        keys.push_back(copy.dstOffset << 32 | copy.size);
    }
    if (!std::is_sorted(keys.begin(), keys.end())) {
        std::sort(keys.begin(), keys.end());
    }
    u64 end = 0;
    for (const u64 key : keys) {
        const u64 begin = key >> 32;
        if (begin < end) {
            return true;
        }
        end = std::max<u64>(end, begin + (key & 0xFFFF'FFFFull));
    }
    return false;
}

/// A/B switch 0 (2026-11-02; ~2% of stage B at 320x240 went to the sort above): the same answer
/// without a sort. The copy shader's regions are whole dwords (offset and size multiples of 4),
/// so marking each region's dwords in a bitmap over the call's destination span finds a byte
/// overlap exactly. A region not in whole dwords, or a span over 4 MiB, takes the sort. The
/// bitmap is all zero between calls: the words a call marked are cleared before it returns.
bool DestinationsOverlapBitmap(std::span<const vk::BufferCopy> copies) {
    static std::vector<u64> bits; // recording owner only
    u64 lo = ~u64{0}, hi = 0, misaligned = 0;
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        lo = std::min<u64>(lo, copy.dstOffset);
        hi = std::max<u64>(hi, copy.dstOffset + copy.size);
        misaligned |= (copy.dstOffset | copy.size) & 3;
    }
    if (hi <= lo) {
        return false; // no region with bytes
    }
    if (misaligned || hi - lo > (u64{4} << 20)) {
        return DestinationsOverlapPacked(copies);
    }
    const u64 words = ((hi - lo) / 4 + 63) / 64;
    if (bits.size() < words) {
        bits.resize(words);
    }
    // Calls f(word, mask) for the bitmap words of the dwords [first, last).
    const auto for_words = [](u64 first, u64 last, auto&& f) {
        const u64 w0 = first / 64, w1 = (last - 1) / 64;
        for (u64 w = w0; w <= w1; ++w) {
            const u64 from = w == w0 ? first % 64 : 0;
            const u64 to = w == w1 ? (last - 1) % 64 + 1 : 64;
            const u64 high = to == 64 ? ~u64{0} : (u64{1} << to) - 1;
            if (!f(w, high & ~((u64{1} << from) - 1))) {
                return false;
            }
        }
        return true;
    };
    size_t done = 0;
    bool overlap = false;
    for (; done < copies.size(); ++done) {
        const auto& copy = copies[done];
        if (copy.size == 0) {
            continue;
        }
        const u64 first = (copy.dstOffset - lo) / 4, last = first + copy.size / 4;
        if (!for_words(first, last, [&](u64 w, u64 m) { return (bits[w] & m) == 0; })) {
            overlap = true;
            break;
        }
        for_words(first, last, [&](u64 w, u64 m) {
            bits[w] |= m;
            return true;
        });
    }
    // The regions marked (before an overlap stopped the pass): their whole words back to zero.
    for (size_t i = 0; i < done; ++i) {
        const auto& copy = copies[i];
        if (copy.size == 0) {
            continue;
        }
        const u64 first = (copy.dstOffset - lo) / 4;
        for_words(first, first + copy.size / 4, [&](u64 w, u64) {
            bits[w] = 0;
            return true;
        });
    }
    return overlap;
}

bool DestinationsOverlap(std::span<const vk::BufferCopy> copies) {
    switch (OverlapMode()) {
    case '1':
        return FastRender::AbOn(0) ? DestinationsOverlapBitmap(copies)
                                   : DestinationsOverlapPacked(copies);
    case 'b': {
        // VR_HLE_OVERLAP=b: the bitmap's answer checked against the key sort's on every call.
        const bool packed = DestinationsOverlapPacked(copies);
        const bool bitmap = DestinationsOverlapBitmap(copies);
        static u64 calls = 0, mismatches = 0, overlaps = 0; // recording owner only
        ++calls;
        overlaps += packed ? 1 : 0;
        if (packed != bitmap && ++mismatches <= 16) {
            LOG_ERROR(Render_Vulkan,
                      "VR_HLE_OVERLAP bitmap verify: mismatch #{}: key sort {} bitmap {} ({} "
                      "regions)",
                      mismatches, packed, bitmap, copies.size());
        }
        if ((calls & 0x3FFF) == 0) {
            LOG_INFO(Render_Vulkan,
                     "VR_HLE_OVERLAP bitmap verify: {} calls, {} overlapping, {} mismatches", calls,
                     overlaps, mismatches);
        }
        return packed;
    }
    case 'v': {
        const bool sorted = DestinationsOverlapSorted(copies);
        const bool packed = DestinationsOverlapPacked(copies);
        static u64 calls = 0, mismatches = 0; // recording owner only
        ++calls;
        if (sorted != packed && ++mismatches <= 16) {
            LOG_ERROR(Render_Vulkan,
                      "VR_HLE_OVERLAP verify: mismatch #{}: pair sort {} key sort {} ({} regions)",
                      mismatches, sorted, packed, copies.size());
        }
        if ((calls & 0xFFFF) == 0) {
            LOG_INFO(Render_Vulkan, "VR_HLE_OVERLAP verify: {} calls, {} mismatches", calls,
                     mismatches);
        }
        return sorted;
    }
    case 'a': {
        static u64 calls = 0, sorted_ticks = 0, packed_ticks = 0; // recording owner only
        const bool use_packed = (++calls & 1) != 0;
        const u64 start = Common::FencedRDTSC();
        const bool overlap = use_packed ? DestinationsOverlapPacked(copies)
                                        : DestinationsOverlapSorted(copies);
        (use_packed ? packed_ticks : sorted_ticks) += Common::FencedRDTSC() - start;
        if ((calls & 0xFFFF) == 0) {
            LOG_INFO(Render_Vulkan,
                     "VR_HLE_OVERLAP timing: {} calls, pair sort {:.0f} cycles a call, key sort "
                     "{:.0f}",
                     calls, double(sorted_ticks) / double(calls / 2),
                     double(packed_ticks) / double(calls / 2));
        }
        return overlap;
    }
    default:
        return DestinationsOverlapSorted(copies);
    }
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
        BumpLogCounter(stats.verify_skipped);
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
        BumpLogCounter(stats.verify_skipped);
        return false;
    }
    if (!allocation_info.pMappedData) {
        vmaDestroyBuffer(allocator, readback, allocation);
        in_flight.store(false, std::memory_order_release);
        BumpLogCounter(stats.verify_skipped);
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
        BumpLogCounter(stats.verify_with_before, with_before);
        BumpLogCounter(stats.verify_changed, changed);
        BumpLogCounter(stats.verified);
        vmaDestroyBuffer(allocator, readback, allocation);
        in_flight.store(false, std::memory_order_release);
    });
}

} // namespace

// FAST_HLE_MULTI_COPY=1 (opt-in; read once at start-up): a grouped call (bit 57, case None) as
// one compute dispatch (vr_hle_multi_copy.comp) instead of one vkCmdCopyBuffer per (source,
// destination) pair and eye. After bbport's buffer_multi_copy.comp (toggle 1 << 43), which
// dispatches per pair: bbport paid ~37 ns of GPU per region in ~1024-region commands; here the
// regions are already merged (NativeStereo::Copy coalescing) and the cost is per command, ~3.2
// pairs per call at 53 calls per frame, so the whole call is one dispatch over up to 8 pairs.
// Right-eye twins, more pairs, unaligned or oversized ranges keep the copy commands.
const bool g_multi_copy = [] {
    const char* value = std::getenv("FAST_HLE_MULTI_COPY");
    return value && value[0] == '1';
}();
constexpr u32 MultiCopyPairs = 8;
constexpr u64 MultiCopyChunkDwords = 4096; // 16 KiB per workgroup at most

struct MultiCopyPipeline {
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::DeviceSize max_range{};
    bool failed{};
};

MultiCopyPipeline& MultiCopyPipelineFor(const Instance& instance) {
    static MultiCopyPipeline pipe = [&instance] {
        MultiCopyPipeline p;
        const auto physical = instance.GetPhysicalDevice();
        p.max_range = physical.getProperties().limits.maxStorageBufferRange;
        if (!physical.getFeatures().shaderStorageBufferArrayDynamicIndexing) {
            p.failed = true;
            return p;
        }
        const auto device = instance.GetDevice();
        const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
            {0, vk::DescriptorType::eStorageBuffer, MultiCopyPairs,
             vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageBuffer, MultiCopyPairs,
             vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        auto set_layout = device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = static_cast<u32>(bindings.size()),
             .pBindings = bindings.data()});
        if (set_layout.result != vk::Result::eSuccess) {
            p.failed = true;
            return p;
        }
        p.set_layout = std::move(set_layout.value);
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
        auto layout = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                         .pSetLayouts = &*p.set_layout,
                                                         .pushConstantRangeCount = 1,
                                                         .pPushConstantRanges = &push});
        if (layout.result != vk::Result::eSuccess) {
            p.failed = true;
            return p;
        }
        p.layout = std::move(layout.value);
        const auto module = Compile(HostShaders::VR_HLE_MULTI_COPY_COMP,
                                    vk::ShaderStageFlagBits::eCompute, device);
        if (!module) {
            p.failed = true;
            return p;
        }
        auto pipeline = device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                           .module = module,
                           .pName = "main"},
                 .layout = *p.layout});
        device.destroyShaderModule(module);
        if (pipeline.result != vk::Result::eSuccess) {
            p.failed = true;
            return p;
        }
        p.pipeline = std::move(pipeline.value);
        LOG_INFO(Render_Vulkan, "FAST_HLE_MULTI_COPY: one dispatch per copy shader call");
        return p;
    }();
    return pipe;
}

/// FAST_HLE_MULTI_COPY: records the call's groups as one dispatch; false leaves them to the copy
/// commands (nothing recorded then). The caller's barriers include the compute stage.
bool MultiCopyCall(Rasterizer& rasterizer, std::span<const CopyGroup> groups,
                   std::span<const vk::BufferCopy> regions) {
    if (groups.empty() || groups.size() > MultiCopyPairs) {
        return false;
    }
    const auto& instance = rasterizer.GetInstance();
    auto& pipe = MultiCopyPipelineFor(instance);
    if (pipe.failed) {
        return false;
    }
    const vk::DeviceSize align = instance.StorageMinAlignment();
    std::array<vk::DescriptorBufferInfo, MultiCopyPairs> sources{}, destinations{};
    thread_local std::vector<u32> table;
    table.clear();
    for (u32 pair = 0; pair < groups.size(); ++pair) {
        const auto& group = groups[pair];
        // A right-eye twin needs its own copy (NativeStereo::Copy): keep the copy commands.
        if (NativeStereo::ExistingRight(group.src) || NativeStereo::ExistingRight(group.dst)) {
            return false;
        }
        const auto group_regions = regions.subspan(group.first, group.count);
        u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
        for (const auto& r : group_regions) {
            if ((r.srcOffset | r.dstOffset | r.size) & 3) {
                return false; // dword copies only
            }
            src_min = std::min<u64>(src_min, r.srcOffset);
            src_max = std::max<u64>(src_max, r.srcOffset + r.size);
            dst_min = std::min<u64>(dst_min, r.dstOffset);
            dst_max = std::max<u64>(dst_max, r.dstOffset + r.size);
        }
        if (src_max <= src_min || dst_max <= dst_min) {
            return false;
        }
        src_min -= src_min % align;
        dst_min -= dst_min % align;
        if (src_max - src_min > pipe.max_range || dst_max - dst_min > pipe.max_range ||
            (src_max - src_min) / 4 > ~0u || (dst_max - dst_min) / 4 > ~0u) {
            return false;
        }
        sources[pair] = {group.src, src_min, src_max - src_min};
        destinations[pair] = {group.dst, dst_min, dst_max - dst_min};
        // Adjacent regions merged (as NativeStereo::Copy does), then cut into chunks.
        for (size_t i = 0; i < group_regions.size();) {
            const u64 src = group_regions[i].srcOffset, dst = group_regions[i].dstOffset;
            u64 size = group_regions[i].size;
            for (++i; i < group_regions.size() && group_regions[i].srcOffset == src + size &&
                      group_regions[i].dstOffset == dst + size;
                 ++i) {
                size += group_regions[i].size;
            }
            for (u64 done = 0; done < size / 4; done += MultiCopyChunkDwords) {
                const u64 dwords = std::min<u64>(MultiCopyChunkDwords, size / 4 - done);
                table.push_back(static_cast<u32>((src - src_min) / 4 + done));
                table.push_back(static_cast<u32>((dst - dst_min) / 4 + done));
                table.push_back(static_cast<u32>(dwords));
                table.push_back(pair);
            }
        }
    }
    const u32 count = static_cast<u32>(table.size() / 4);
    if (count == 0 || count > 65535) {
        return false;
    }
    // Unused slots repeat the first pair: every element of a statically used array is valid.
    for (size_t pair = groups.size(); pair < MultiCopyPairs; ++pair) {
        sources[pair] = sources[0];
        destinations[pair] = destinations[0];
    }
    auto& stream = rasterizer.GetBufferCache().GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
    const u64 table_bytes = table.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(table.data(), table_bytes, align);
    const vk::DescriptorBufferInfo table_info{stream.Handle(), table_offset, table_bytes};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0,
         .descriptorCount = MultiCopyPairs,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = sources.data()},
        {.dstBinding = 1,
         .descriptorCount = MultiCopyPairs,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = destinations.data()},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &table_info},
    }};
    const auto cmdbuf = rasterizer.GetScheduler().Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
    ScratchPush::Invalidate();
    cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                         &count);
    cmdbuf.dispatch(count, 1, 1);
    return true;
}

// FAST_HLE_COPY_MIRROR (default on, '0' off; 2026-11-01, faces exploding on 0.8.3 and 0.8.5): a
// face's FaceGen vertex stream is written once, when its character loads, by this copy shader
// from the game's memory. With readbacks off only the GPU held those bytes, so any later upload
// of the game's stale copy (a write fault beside them, a kept page dropped from the full pool, an
// emulator write) sent the face's triangles to infinity until the next load. A call into a
// destination no call wrote in the last MirrorFrames frames (a load's, a character appearing) is
// also done in the game's memory, as the PS4's GPU does: from then on its copy is the GPU's. The
// calls that rewrite the same destinations every frame (~30 a frame, ~0.7 MB) are only looked up.
static const bool g_copy_mirror = [] {
    const char* value = std::getenv("FAST_HLE_COPY_MIRROR");
    return !(value && value[0] == '0');
}();

static void MirrorNewCopies(VideoCore::BufferCache& buffer_cache, VAddr src_base, VAddr dst_base,
                            std::span<const vk::BufferCopy> copies) {
    // A call's destination (its first copy's), the last frame a call wrote it and that frame's
    // verdict, so a frame's later calls there get its first one's: one probe a call in an
    // open-addressed table (stage B only, as `copies`). A half-full one is emptied, never grown:
    // each destination is new once.
    struct Seen {
        VAddr dst;
        u64 frame;
        bool mirror;
    };
    static constexpr size_t Slots = size_t{1} << 14;
    static std::array<Seen, Slots> seen{};
    static size_t used = 0;
    static constexpr u64 MirrorFrames = 120; // a previous area's destinations are older
    if (copies.empty()) {
        return;
    }
    const VAddr key = dst_base + copies.front().dstOffset;
    const u64 frame = buffer_cache.GcTick();
    size_t slot = static_cast<size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> 50);
    while (seen[slot].dst != 0 && seen[slot].dst != key) {
        slot = (slot + 1) & (Slots - 1);
    }
    Seen& last = seen[slot];
    if (last.dst == 0) {
        if (++used > Slots / 2) {
            seen.fill({});
            used = 1;
        }
        last = {key, frame, true};
    } else if (last.frame != frame) {
        last.mirror = frame - last.frame > MirrorFrames;
        last.frame = frame;
    }
    if (!last.mirror) {
        return;
    }
    // A source the GPU wrote is stale in the game's memory: nothing to copy from there.
    VAddr src_lo = ~VAddr{0}, src_hi = 0;
    for (const auto& c : copies) {
        src_lo = std::min<VAddr>(src_lo, c.srcOffset);
        src_hi = std::max<VAddr>(src_hi, c.srcOffset + c.size);
    }
    if (buffer_cache.IsRegionGpuModified(src_base + src_lo, src_hi - src_lo)) {
        return;
    }
    // Neighbouring copies (contiguous on both sides) go as one write.
    auto* memory = Core::Memory::Instance();
    for (size_t i = 0; i < copies.size();) {
        u64 src = copies[i].srcOffset, dst = copies[i].dstOffset, size = copies[i].size;
        for (++i; i < copies.size() && copies[i].srcOffset == src + size &&
                  copies[i].dstOffset == dst + size;
             ++i) {
            size += copies[i].size;
        }
        const VAddr from = src_base + src, to = dst_base + dst;
        if (to < from + size && from < to + size) {
            continue; // overlapping: the GPU's order of bytes is not a plain copy's
        }
        memory->TryWriteBacking(std::bit_cast<void*>(to), std::bit_cast<const void*>(from), size);
    }
}

// The copy sink (on unless FAST_HLE_COPY_SINK=0, read once, or in-run A/B switch 13 cleared,
// fast_render.h AbOn; the study: scratch copy-study, 2026-11-16). The game runs this copy shader
// ~35 times a frame inside its render passes (12 times inside the G-buffer in Central Yharnam), and
// each call ended the pass for its copies, so the next draw began it again. No draw of the pass
// reads what those copies write: their destinations are read in later passes, or written again
// first. So a call that comes while a pass is open, and would be grouped (bit 57), keeps the pass
// open: its lookups are made at once, as today (they must record nothing), and its copies wait
// for the pass's end. Scheduler::EndRenderingNow then records every waiting call, in order, right
// after vkCmdEndRendering, between the calls' own two barriers (and a transfer barrier between
// calls whose bytes may meet). Moving a copy later changes nothing as long as nothing in between
// touches its bytes, and everything that touches guest buffer memory on the GPU goes through
// BufferCache::MaterializeRetiles first (the re-tile deferral relies on the same rule): while
// copies wait, that hook ends the pass, and so records them, before any access that meets a
// waiting destination, or a waiting source read from a cache buffer. A source the lookup copied
// into the stream ring (nearly all of them: the game's sources are small and CPU-written) is a
// snapshot nothing later writes, so it is not watched. The waiting bytes' 4 KiB pages are marked
// in a bitmap; an access tests 64 pages per load and searches the exact byte ranges only on a
// marked page. Nothing waits outside an open pass: every pass end records the copies, a submit
// ends the pass first, and a buffer merge or the stream ring's reuse of the open tick both end it
// too. The copies' buffers stay alive until their command buffer completes (deleted buffers are
// destroyed only after their tick). FAST_CPU_STAGEB_SPLIT=2 (B2 encodes draws on another thread)
// and the copy shader's test modes keep today's path. Recording thread only.
namespace CopySink {

const bool g_enabled = [] {
    const char* value = std::getenv("FAST_HLE_COPY_SINK");
    return !(value && value[0] == '0');
}();
constexpr unsigned AbSwitch = 13; // FastRender::AbOn

/// One bit per 4 KiB page of the 40-bit guest address space, in 1 GiB chunks made on first use and
/// kept (the copy shader's destinations lie in a few of them). Every index is checked: a range
/// past the 40 bits is never marked and always counts as marked when tested.
class PageBits {
public:
    static constexpr u32 PageShift = 12;
    static constexpr u32 ChunkShift = 30;
    static constexpr u64 PagesPerChunk = u64{1} << (ChunkShift - PageShift);
    static constexpr u64 WordsPerChunk = PagesPerChunk / 64;
    static constexpr u64 NumChunks = u64{1} << (40 - ChunkShift);
    static constexpr VAddr Limit = VAddr{1} << 40;
    /// A tested range over this many pages is not scanned: the caller searches it exactly.
    static constexpr u64 MaxScanPages = 64 * 64;

    /// Marks [lo, hi); false if it lies past the limit (nothing marked then).
    bool Mark(VAddr lo, VAddr hi) {
        if (hi <= lo) {
            return true;
        }
        if (hi > Limit) {
            return false;
        }
        Apply<true>(lo, hi);
        return true;
    }
    /// Clears [lo, hi) (a range Mark took).
    void Clear(VAddr lo, VAddr hi) {
        if (hi > lo && hi <= Limit) {
            Apply<false>(lo, hi);
        }
    }
    /// Whether a page of [lo, hi) may be marked.
    bool Any(VAddr lo, VAddr hi) const {
        if (hi <= lo) {
            return false;
        }
        if (hi > Limit) {
            return true;
        }
        const u64 first = lo >> PageShift, last = (hi - 1) >> PageShift;
        if (last - first >= MaxScanPages) {
            return true;
        }
        for (u64 page = first; page <= last;) {
            const u64 chunk = page / PagesPerChunk;
            const u64 chunk_last = std::min(last, (chunk + 1) * PagesPerChunk - 1);
            if (const auto* words = chunks[chunk].get()) {
                for (u64 p = page; p <= chunk_last;) {
                    const u64 bit = p % 64;
                    const u64 count = std::min<u64>(64 - bit, chunk_last - p + 1);
                    const u64 mask = (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
                    if (((*words)[(p % PagesPerChunk) / 64] & mask) != 0) {
                        return true;
                    }
                    p += count;
                }
            }
            page = chunk_last + 1;
        }
        return false;
    }

private:
    template <bool set>
    void Apply(VAddr lo, VAddr hi) {
        const u64 first = lo >> PageShift, last = (hi - 1) >> PageShift;
        for (u64 page = first; page <= last;) {
            const u64 chunk = page / PagesPerChunk;
            const u64 chunk_last = std::min(last, (chunk + 1) * PagesPerChunk - 1);
            auto& words = chunks[chunk];
            if constexpr (set) {
                if (!words) {
                    words = std::make_unique<std::array<u64, WordsPerChunk>>();
                }
            }
            if (words) {
                for (u64 p = page; p <= chunk_last;) {
                    const u64 bit = p % 64;
                    const u64 count = std::min<u64>(64 - bit, chunk_last - p + 1);
                    const u64 mask = (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
                    u64& word = (*words)[(p % PagesPerChunk) / 64];
                    word = set ? (word | mask) : (word & ~mask);
                    p += count;
                }
            }
            page = chunk_last + 1;
        }
    }

    std::array<std::unique_ptr<std::array<u64, WordsPerChunk>>, NumChunks> chunks{};
};

using Intervals = std::vector<std::pair<VAddr, VAddr>>;

/// Whether [lo, hi) meets an interval of sorted, merged `v`.
bool Meets(const Intervals& v, VAddr lo, VAddr hi) {
    const auto it = std::upper_bound(
        v.begin(), v.end(), lo,
        [](VAddr a, const std::pair<VAddr, VAddr>& interval) { return a < interval.second; });
    return it != v.end() && it->first < hi;
}

/// Adds [lo, hi) into sorted, merged `into` as MergeInto does: an interval it overlaps or touches
/// joins it. `into` is merged, so its intervals are disjoint and sorted by both ends: those from
/// the first that ends at or after `lo` to the last that starts at or before `hi` all join.
void InsertInterval(Intervals& into, VAddr lo, VAddr hi) {
    const auto first = std::lower_bound(
        into.begin(), into.end(), lo,
        [](const std::pair<VAddr, VAddr>& interval, VAddr v) { return interval.second < v; });
    auto last = first;
    while (last != into.end() && last->first <= hi) {
        ++last;
    }
    if (first == last) {
        into.insert(first, {lo, hi});
        return;
    }
    first->first = std::min(first->first, lo);
    first->second = std::max((last - 1)->second, hi);
    into.erase(first + 1, last);
}

/// Adds `add` (any order) into sorted, merged `into`; `scratch` keeps its capacity between calls.
void MergeInto(Intervals& into, Intervals& add, Intervals& scratch) {
    if (add.empty()) {
        return;
    }
    // A call's few ranges (most often one): each into its place, no sort and no rebuild of the
    // waiting list (2026-11-17's profile: the sort and the rebuild ~0.6% of stage B in the
    // Forbidden Woods). The same intervals as the merge below, which takes the longer lists.
    if (add.size() <= 4) {
        for (const auto& [lo, hi] : add) {
            InsertInterval(into, lo, hi);
        }
        return;
    }
    std::sort(add.begin(), add.end());
    scratch.clear();
    std::merge(into.begin(), into.end(), add.begin(), add.end(), std::back_inserter(scratch));
    into.clear();
    for (const auto& interval : scratch) {
        if (!into.empty() && interval.first <= into.back().second) {
            into.back().second = std::max(into.back().second, interval.second);
        } else {
            into.push_back(interval);
        }
    }
}

/// One (source, destination) buffer pair of a waiting call, as bit 57's phase 2 groups them.
struct Group {
    vk::Buffer src;
    vk::Buffer dst;
    u32 first{};
    u32 count{};
    bool barrier_before{}; // the call's bytes may meet an earlier waiting call's
};

struct State {
    Scheduler* scheduler{}; // the game's, which the waiting calls end the pass of
    std::vector<vk::BufferCopy> regions; // the waiting copies, rebased, in call order
    std::vector<Group> groups;
    Intervals dst, src, add_dst, add_src, scratch; // waiting guest bytes, sorted and merged
    PageBits pages;
    bool pages_overflow{}; // a waiting range past the bitmap: every access is searched
    u32 calls{};           // calls waiting
    bool muted{};          // a call's own lookups
    bool flushing{};
    // The 'FAST hle_copy_sink' line (log builds).
    u64 deferred{}, flushes{}, access_flushes{}, closed{}, recorded{}, overlap{}, reported{};
};

State& Get() {
    static State state;
    return state;
}

constexpr vk::MemoryBarrier BeforeCopies{
    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
    .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
};
constexpr vk::MemoryBarrier AfterCopies{
    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
};

void Report(State& st) {
    if constexpr (Common::Log::Enabled) {
        if (st.deferred - st.reported < 65536) {
            return;
        }
        st.reported = st.deferred;
        LOG_INFO(Render_Vulkan,
                 "FAST hle_copy_sink: {} calls kept for the pass end, {} flushes ({} by a buffer "
                 "access); today's path: pass closed {}, lookups recorded {}, overlapping {}",
                 st.deferred, st.flushes, st.access_flushes, st.closed, st.recorded, st.overlap);
    }
}

/// Scheduler::EndRenderingNow's hook (armed while calls wait): the pass has just ended.
void Flush(Scheduler& scheduler) {
    State& st = Get();
    scheduler.SetPassEndHook(nullptr);
    VideoCore::BufferCache::copy_sink_hook = nullptr;
    if (st.calls == 0 || st.flushing) {
        return;
    }
    st.flushing = true;
    if constexpr (Common::Log::Enabled) {
        ++st.flushes;
    }
    GpuPassProfiler::MarkAt(scheduler, 0xC0B1'0000'0000ull ^ COPY_SHADER_HASH,
                            GpuPassProfiler::Group::HleCopy,
                            "copy shader fefebf9f HLE (barriers, buffer lookups, copies)");
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           BeforeCopies, {}, {});
    for (size_t i = 0; i < st.groups.size(); ++i) {
        const Group& group = st.groups[i];
        if (group.barrier_before && i != 0) {
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, BeforeCopies, {}, {});
        }
        NativeStereo::Copy(scheduler, group.src, group.dst,
                           std::span{st.regions}.subspan(group.first, group.count), true);
    }
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, AfterCopies, {}, {});
    for (const auto& [lo, hi] : st.dst) {
        st.pages.Clear(lo, hi);
    }
    for (const auto& [lo, hi] : st.src) {
        st.pages.Clear(lo, hi);
    }
    st.regions.clear();
    st.groups.clear();
    st.dst.clear();
    st.src.clear();
    st.pages_overflow = false;
    st.calls = 0;
    st.flushing = false;
}

/// BufferCache::MaterializeRetiles' hook (armed while calls wait): an access to guest buffer
/// memory. One meeting waiting bytes ends the pass first, and the pass end records the copies.
void Access(VAddr addr, u64 size) {
    State& st = Get();
    if (st.muted || st.flushing || st.calls == 0 || size == 0) {
        return;
    }
    const VAddr end = size > ~u64{0} - addr ? ~u64{0} : addr + size;
    if (!st.pages_overflow && !st.pages.Any(addr, end)) {
        return;
    }
    if (!Meets(st.dst, addr, end) && !Meets(st.src, addr, end)) {
        return;
    }
    if constexpr (Common::Log::Enabled) {
        ++st.access_flushes;
    }
    st.scheduler->EndRendering();
}

/// Keeps a call's copies (relative to the sharps' bases) for the end of the open pass. False: the
/// caller takes today's path (whose EndRendering records any waiting calls first).
bool TryKeep(Rasterizer& rasterizer, VAddr src_base, VAddr dst_base,
             std::span<vk::BufferCopy> copies) {
    State& st = Get();
    auto& scheduler = rasterizer.GetScheduler();
    auto& buffer_cache = rasterizer.GetBufferCache();
    // Whether the call's bytes may meet a waiting call's (asked before the regions are rebased).
    bool meets_waiting = false;
    if (st.calls != 0) {
        for (const auto& c : copies) {
            if (c.size != 0 &&
                (st.pages_overflow ||
                 st.pages.Any(src_base + c.srcOffset, src_base + c.srcOffset + c.size) ||
                 st.pages.Any(dst_base + c.dstOffset, dst_base + c.dstOffset + c.size))) {
                meets_waiting = true;
                break;
            }
        }
    }
    // Bit 57's phase 1 with the pass open: every batch's lookup, which must record nothing (an
    // upload, a migration, a buffer made or dropped). A chain onto waiting bytes keeps its order:
    // the call waits behind them.
    const u64 command_uses = scheduler.CommandUses();
    const u64 tick = scheduler.CurrentTick();
    const u64 epoch = buffer_cache.CopyMemoEpoch();
    static std::vector<LookedUp> looked_up;
    looked_up.clear();
    st.muted = true;
    for (u32 begin = 0; begin < copies.size();) {
        const CopyBatch batch = NextBatch(copies, begin, 16 * 1024);
        const auto [src_buf, src_offset, dst_buf, dst_offset] = rasterizer.ObtainCopyBuffers(
            src_base + batch.src_min, static_cast<u32>(batch.src_max - batch.src_min),
            dst_base + batch.dst_min, static_cast<u32>(batch.dst_max - batch.dst_min));
        looked_up.push_back(LookedUp{.batch = batch,
                                     .src = src_buf->Handle(),
                                     .dst = dst_buf->Handle(),
                                     .src_offset = src_offset,
                                     .dst_offset = dst_offset});
        begin = batch.end;
    }
    st.muted = false;
    if (scheduler.CommandUses() != command_uses || scheduler.CurrentTick() != tick ||
        buffer_cache.CopyMemoEpoch() != epoch || !scheduler.IsRendering()) {
        if constexpr (Common::Log::Enabled) {
            ++st.recorded;
        }
        return false;
    }
    if (SourceIsDestination(looked_up) || DestinationsOverlap(copies)) {
        if constexpr (Common::Log::Enabled) {
            ++st.overlap;
        }
        return false;
    }
    // The waiting bytes: every destination, and the sources read from a cache buffer.
    const vk::Buffer stream =
        buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).Handle();
    st.add_dst.clear();
    st.add_src.clear();
    for (const auto& l : looked_up) {
        for (u32 i = l.batch.begin; i < l.batch.end; ++i) {
            const auto& c = copies[i];
            if (c.size == 0) {
                continue;
            }
            st.add_dst.emplace_back(dst_base + c.dstOffset, dst_base + c.dstOffset + c.size);
            if (l.src != stream) {
                st.add_src.emplace_back(src_base + c.srcOffset, src_base + c.srcOffset + c.size);
            }
        }
    }
    for (const auto& [lo, hi] : st.add_dst) {
        st.pages_overflow |= !st.pages.Mark(lo, hi);
    }
    for (const auto& [lo, hi] : st.add_src) {
        st.pages_overflow |= !st.pages.Mark(lo, hi);
    }
    MergeInto(st.dst, st.add_dst, st.scratch);
    MergeInto(st.src, st.add_src, st.scratch);
    // Rebased into the batches' buffers and grouped by buffer pair, as bit 57's phase 2.
    for (const auto& l : looked_up) {
        for (u32 i = l.batch.begin; i < l.batch.end; ++i) {
            copies[i].srcOffset = copies[i].srcOffset - l.batch.src_min + l.src_offset;
            copies[i].dstOffset = copies[i].dstOffset - l.batch.dst_min + l.dst_offset;
        }
    }
    bool first_group = true;
    for (size_t first = 0; first < looked_up.size(); ++first) {
        if (looked_up[first].grouped) {
            continue;
        }
        const auto start = static_cast<u32>(st.regions.size());
        for (size_t i = first; i < looked_up.size(); ++i) {
            auto& l = looked_up[i];
            if (l.grouped || l.src != looked_up[first].src || l.dst != looked_up[first].dst) {
                continue;
            }
            l.grouped = true;
            st.regions.insert(st.regions.end(), copies.begin() + l.batch.begin,
                              copies.begin() + l.batch.end);
        }
        st.groups.push_back(Group{.src = looked_up[first].src,
                                  .dst = looked_up[first].dst,
                                  .first = start,
                                  .count = static_cast<u32>(st.regions.size()) - start,
                                  .barrier_before = first_group && meets_waiting});
        first_group = false;
    }
    if (st.calls++ == 0) {
        st.scheduler = &scheduler;
        scheduler.SetPassEndHook(&Flush);
        VideoCore::BufferCache::copy_sink_hook = &Access;
    }
    // FAST_CPU_REPEAT_DRAW: the next draw may not take the last one's bindings unchecked (nothing
    // was recorded, but the bytes it binds may now wait).
    rasterizer.ForgetRepeatDraw();
    if constexpr (Common::Log::Enabled) {
        ++st.deferred;
        Report(st);
    }
    return true;
}

/// Whether this call may wait (read per call; the switches may change between calls).
bool MayKeep(const Scheduler& scheduler, size_t copies, bool stats) {
    static const bool test_modes = std::getenv("VR_COPY_MERGE_KB") != nullptr ||
                                   std::getenv("FAST_HLE_COPY_GROUPS_VERIFY") != nullptr;
    if (!g_enabled || !FastRender::AbOn(AbSwitch) || copies == 0 || copies > 64 * 1024 ||
        stats || g_multi_copy || test_modes || scheduler.SplitThreaded() ||
        !FastRender::On(HleCopyGroups)) {
        return false;
    }
    if (!scheduler.IsRendering()) {
        if constexpr (Common::Log::Enabled) {
            ++Get().closed;
        }
        return false;
    }
    return true;
}

} // namespace CopySink

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
    if (g_copy_mirror) {
        MirrorNewCopies(buffer_cache, src_buf_sharp.base_address, dst_buf_sharp.base_address,
                        copies);
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

    // The copy sink: a call that comes while a pass is open may keep the pass open.
    if (CopySink::MayKeep(scheduler, copies.size(), stats) &&
        CopySink::TryKeep(rasterizer, src_buf_sharp.base_address, dst_buf_sharp.base_address,
                          std::span{copies})) {
        return true;
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
    // FAST_HLE_MULTI_COPY: the call's outer barriers also cover the compute stage (its dispatch
    // reads and writes as shader accesses); the inner ones stay transfer-only (that path's copies).
    static constexpr vk::MemoryBarrier MULTI_READ_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite |
                         vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
    };
    static constexpr vk::MemoryBarrier MULTI_WRITE_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    constexpr auto multi_stages =
        vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eComputeShader;
    scheduler.Commands().pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        g_multi_copy ? multi_stages : vk::PipelineStageFlags{vk::PipelineStageFlagBits::eTransfer},
        vk::DependencyFlagBits::eByRegion, g_multi_copy ? MULTI_READ_BARRIER : READ_BARRIER, {},
        {});
    // FAST_HLE_COPY_SPLIT=1 (diagnostic, with bit 38): the call in three labels, so the GPU time of
    // the pre-barrier's drain, the copies and the post-barrier's drain show apart.
    static const bool split_profile = [] {
        const char* value = std::getenv("FAST_HLE_COPY_SPLIT");
        return value && value[0] == '1';
    }();
    if (split_profile)
        GpuPassProfiler::MarkAt(scheduler, 0xC0B1'0001'0000ull ^ COPY_SHADER_HASH,
                                GpuPassProfiler::Group::HleCopy,
                                "copy shader fefebf9f HLE: copies (after the pre-barrier)");

    // bbport's copy-span experiment: preserve every copy and its order while
    // avoiding synchronization of very large holes between small copied ranges.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* value = std::getenv("VR_COPY_MERGE_KB");
        if (!value) return vk::DeviceSize(16 * 1024);
        char* end = nullptr;
        const auto kb = std::strtoul(value, &end, 10);
        if (*end || (kb != 16 && kb != 64 && kb != 256 && kb != 65536))
            return vk::DeviceSize(16 * 1024);
        LOG_INFO(Render_Vulkan, "Copy merge span: {} KiB", kb);
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
    const u64 group_calls = BumpLogCounter(group_stats.calls);
    if (!FastRender::On(HleCopyGroups)) {
        BumpLogCounter(group_stats.bit_off);
        record_interleaved({}, stats);
    } else if (copies.empty() || copies.size() > 64 * 1024) {
        // Nothing to group, or one recorder packet per eye would grow past a few MB.
        BumpLogCounter(group_stats.skipped);
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
            BumpLogCounter(group_stats.recorded);
            scheduler.Commands().pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});
            record_interleaved({}, false);
            break;
        case GroupFallback::Alias:
        case GroupFallback::Overlap:
            // Copies that may depend on each other: today's commands, from the lookups made.
            BumpLogCounter(why == GroupFallback::Alias ? group_stats.alias : group_stats.overlap);
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
            BumpLogCounter(group_stats.batches, looked_up.size());
            BumpLogCounter(group_stats.copy_calls, groups.size());
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
            if (g_multi_copy && check) {
                // The dispatch overwrites what Begin's transfer reads just read.
                const vk::MemoryBarrier2 before_dispatch{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                };
                scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
                    .memoryBarrierCount = 1, .pMemoryBarriers = &before_dispatch});
            }
            const bool dispatched = g_multi_copy && MultiCopyCall(rasterizer, groups, gathered);
            if (g_multi_copy) {
                BumpLogCounter(dispatched ? group_stats.multi_dispatched : group_stats.multi_fallback);
            }
            if (!dispatched) {
                for (const auto& group : groups) {
                    NativeStereo::Copy(scheduler, group.src, group.dst,
                                       std::span{gathered}.subspan(group.first, group.count),
                                       true);
                }
            }
            if (check) {
                if (dispatched) {
                    // Finish reads with transfer commands what the dispatch wrote.
                    const vk::MemoryBarrier2 after_dispatch{
                        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                    };
                    scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
                        .memoryBarrierCount = 1, .pMemoryBarriers = &after_dispatch});
                }
                readback.Finish(rasterizer, groups, gathered);
            }
            break;
        }
        }
    }
    ReportGroups(group_calls);

    if (split_profile)
        GpuPassProfiler::MarkAt(scheduler, 0xC0B1'0002'0000ull ^ COPY_SHADER_HASH,
                                GpuPassProfiler::Group::HleCopy,
                                "copy shader fefebf9f HLE: post-barrier");
    scheduler.Commands().pipelineBarrier(
        g_multi_copy ? multi_stages : vk::PipelineStageFlags{vk::PipelineStageFlagBits::eTransfer},
        vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlagBits::eByRegion,
        g_multi_copy ? MULTI_WRITE_BARRIER : WRITE_BARRIER, {}, {});

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
