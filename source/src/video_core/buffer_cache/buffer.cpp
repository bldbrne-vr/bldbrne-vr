#include "../../../scratchpad/dual-eye-perf/profile_scopes.h"
#include "../../../scratchpad/dual-eye-perf/stream_watch.h"
#include "../../../scratchpad/dual-eye-perf/stream_copy.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/read_barrier_logic.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_transfer_eye_barriers.h"
#include "video_core/renderer_vulkan/vr_perf_logic.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryUsage type) {
    switch (type) {
    case MemoryUsage::Upload:
        return "Upload";
    case MemoryUsage::Download:
        return "Download";
    case MemoryUsage::Stream:
        return "Stream";
    case MemoryUsage::DeviceLocal:
        return "DeviceLocal";
    default:
        return "Invalid";
    }
}

// Where the stream buffer (StreamBuffer with MemoryUsage::Stream: the 64 MB ring that every small
// read-only buffer of every draw is copied into, ~8,600 copies and 7.7 MB per clinic-hall frame)
// keeps its data (documents/bloodborne-vr.md "Performance"). VR_STREAM_MEM:
//   device: upstream's placement, the GPU's BAR when it has one. Its write-combined CPU writes go
//           over PCIe and every later locked instruction waits for them: ~2.3 ms of the command
//           processor per clinic frame (RTX 5080).
//   cached: cached system memory. Plain cached writes; the GPU reads them over PCIe (+1.0 ms of GPU
//           per clinic frame).
//   shadow: the GPU's BAR, written through a cached copy that goes into the BAR in one sequential
//           memcpy before each submit of the scheduler (StreamBuffer::FlushShadow).
StreamMemoryMode GetStreamMemoryMode() {
    static const StreamMemoryMode mode = [] {
        const char* v = std::getenv("VR_STREAM_MEM");
        if (v && std::strcmp(v, "device") == 0) {
            return StreamMemoryMode::Device;
        }
        if (v && std::strcmp(v, "cached") == 0) {
            return StreamMemoryMode::Cached;
        }
        if (v && std::strcmp(v, "shadow") == 0) {
            return StreamMemoryMode::Shadow;
        }
        return StreamMemoryMode::Default;
    }();
    return mode;
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
    return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryUsage::Download:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryUsage::DeviceLocal:
        return {};
    }
    return {};
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
    case MemoryUsage::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::Upload:
    case MemoryUsage::Download:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    if (buffer) {
        NativeStereo::ForgetBuffer(buffer);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
}

void UniqueBuffer::Create(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage,
                          VmaAllocationInfo* out_alloc_info, bool host_cached) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    const VmaAllocationCreateFlags bda_flag =
        with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    // host_cached: mapped, cached system memory (StreamMemoryMode::Cached).
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag |
                 (host_cached ? VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                              : MemoryUsageVmaFlags(usage)),
        .usage = host_cached ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags =
            host_cached ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                        : MemoryUsagePreferredVmaFlags(usage),
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    VkBuffer unsafe_buffer{};
    VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                      &allocation, out_alloc_info);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
               vk::to_string(vk::Result{result}));
    buffer = vk::Buffer{unsafe_buffer};
    native_entry = NativeStereo::RegisterBuffer(buffer, buffer_ci.size, allocator, usage == MemoryUsage::Stream || usage == MemoryUsage::Upload);

    if (with_bda) {
        vk::BufferDeviceAddressInfo bda_info{
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_, MemoryUsage usage_,
               VAddr cpu_addr_, vk::BufferUsageFlags flags, u64 size_bytes_, bool host_cached)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, instance{&instance_}, scheduler{&scheduler_},
      usage{usage_}, buffer{instance->GetDevice(), instance->GetAllocator()} {
    // Create buffer object.
    const vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = flags,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, usage, &alloc_info, host_cached);

    const auto device = instance->GetDevice();
    Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);

    // Map it if it is host visible.
    VkMemoryPropertyFlags property_flags{};
    vmaGetAllocationMemoryProperties(instance->GetAllocator(), buffer.allocation, &property_flags);
    if (alloc_info.pMappedData) {
        mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
    }
    is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

namespace {
namespace RB = ReadBarrierLogic;

// The values read_barrier_logic.h spells out (it has no Vulkan).
static_assert(RB::IndirectCommandRead == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
static_assert(RB::IndexRead == VK_ACCESS_2_INDEX_READ_BIT);
static_assert(RB::VertexAttributeRead == VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
static_assert(RB::UniformRead == VK_ACCESS_2_UNIFORM_READ_BIT);
static_assert(RB::ShaderRead == VK_ACCESS_2_SHADER_READ_BIT);
static_assert(RB::ShaderWrite == VK_ACCESS_2_SHADER_WRITE_BIT);
static_assert(RB::TransferRead == VK_ACCESS_2_TRANSFER_READ_BIT);
static_assert(RB::TransferWrite == VK_ACCESS_2_TRANSFER_WRITE_BIT);
static_assert(RB::HostRead == VK_ACCESS_2_HOST_READ_BIT);
static_assert(RB::MemoryRead == VK_ACCESS_2_MEMORY_READ_BIT);
static_assert(RB::MemoryWrite == VK_ACCESS_2_MEMORY_WRITE_BIT);
static_assert(RB::AllCommands == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
static_assert(RB::AllReadBits == PerfLogic::ReadOnlyAccess);
static_assert(RB::AnyRead == static_cast<VkAccessFlags2>(Vulkan::PassBreaks::AnyRead));
static_assert((RB::AnyRead | RB::HostRead) == static_cast<VkAccessFlags2>(BarrierReadAccess));

u64 Bits(vk::AccessFlags2 access) {
    return static_cast<VkAccessFlags2>(access);
}
u64 Bits(vk::PipelineStageFlags2 stage) {
    return static_cast<VkPipelineStageFlags2>(stage);
}

constexpr std::array<const char*, 4> KindNames{"same", "elided", "broadened", "old"};
constexpr std::array<const char*, static_cast<size_t>(RB::Mismatch::Count)> MismatchNames{
    "none", "no barrier where the old one publishes a write", "a weaker barrier",
    "a diverged state"};

// FAST_GPU_READ_BARRIERS' counts, per recording thread (plain: every request but a repeated read,
// which GetBarrier answers inline, comes through here).
struct ReadBarrierStats {
    u64 requests{};
    std::array<u64, 4> kinds{}; // by RB::Kind (Same: a repeated write)
    u64 image_data{}, transfer{}, same_draw{}, partial{}; // reads that took the old step
    u64 compared{}, read_after_read{}, merged{};
};
thread_local ReadBarrierStats t_read_barrier_stats;
std::atomic<u64> g_read_barrier_mismatches{0}; // FAST_GPU_READ_BARRIERS_VERIFY, every thread
} // namespace

std::optional<vk::BufferMemoryBarrier2> Buffer::GetBarrierReads(vk::AccessFlags2 dst_acess_mask,
                                                                vk::PipelineStageFlagBits2 dst_stage,
                                                                u32 offset) {
    auto& stats = t_read_barrier_stats;
    const bool dst_reads_only = (dst_acess_mask & BarrierReadAccess) == dst_acess_mask;
    RB::State state{Bits(access_mask), static_cast<u64>(stage)};
    RB::Request request{
        .access = Bits(dst_acess_mask),
        .stage = static_cast<u64>(dst_stage),
        .whole = offset == 0,
        .excluded = image_data,
    };
    const bool same = request.access == state.access && request.stage == state.stage;
    bool transfer = false;
    bool same_draw = false;
    if (!dst_reads_only) {
        // Bound for writing by the draw whose bindings are being made (or a copy right now).
        write_request_draw = Vulkan::Perf::DrawSerial();
    } else if (!same && !image_data) {
        // A copy's source (JoinOverlap, TextureCache::RefreshImage): recorded outside a pass, so
        // nothing to save, and inside one copy-shader HLE call it can follow a batch's copy into
        // this buffer before that call's closing barrier.
        constexpr u64 TransferStages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                       VK_PIPELINE_STAGE_2_COPY_BIT;
        transfer = (request.stage & TransferStages) != 0;
        // This draw also writes the buffer, after every barrier of its bindings: not visible yet.
        // Never so in the elidable state: a write request leaves it, and one made earlier by the
        // same draw kept the read that would have entered it on the old step.
        same_draw = !transfer && !RB::Elidable(state) &&
                    write_request_draw == Vulkan::Perf::DrawSerial();
        request.excluded = transfer || same_draw;
    }
    RB::Masks now{};
    const RB::Kind kind = RB::ReadStep(state, request, now);
    RB::Masks recorded = now;
    ++stats.requests;
    ++stats.kinds[static_cast<size_t>(kind)];
    if (kind == RB::Kind::Old && RB::IsPureRead(request.access)) {
        ++(image_data ? stats.image_data
                      : transfer ? stats.transfer : same_draw ? stats.same_draw : stats.partial);
    }
    // The 'FAST pass_breaks' and 'FAST pass_continue' lines' counts (a few per draw at most).
    if (kind == RB::Kind::Elided) {
        Vulkan::PassBreaks::Count(Vulkan::PassBreaks::g_barriers.elided);
    } else if (kind == RB::Kind::Broadened) {
        Vulkan::PassBreaks::Count(Vulkan::PassBreaks::g_barriers.broadened);
    }
    if (Vulkan::PassBreaks::g_read_barriers_verify) [[unlikely]] {
        // The switch-off step on its own copy of the state; the GPU gets both barriers merged.
        RB::State old_state{Bits(verify_access_mask), Bits(verify_stage)};
        const RB::Masks old = RB::OldStep(old_state, request.access, request.stage);
        verify_access_mask = vk::AccessFlags2{old_state.access};
        verify_stage = vk::PipelineStageFlags2{old_state.stage};
        ++stats.compared;
        if (!now.valid && RB::ReadAfterRead(old)) {
            ++stats.read_after_read;
            if (Vulkan::PassBreaks::g_stats_enabled) {
                Vulkan::PassBreaks::Count(Vulkan::PassBreaks::g_barriers.read_after_read);
            }
        }
        if (now.valid && old.valid) {
            ++stats.merged;
        }
        const RB::Mismatch mismatch = RB::Check(now, state, old, old_state);
        if (mismatch != RB::Mismatch::None) [[unlikely]] {
            const u64 n = g_read_barrier_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || n % 1024 == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST read_barriers verify: {} for buffer {:#x}+{:#x}: request access "
                          "{:#x} stage {:#x} ({}), new {}{:#x}/{:#x} -> {:#x}/{:#x} state "
                          "{:#x}/{:#x}, old {}{:#x}/{:#x} -> {:#x}/{:#x} state {:#x}/{:#x}, "
                          "mismatch #{}",
                          MismatchNames[static_cast<size_t>(mismatch)], cpu_addr, size_bytes,
                          request.access, request.stage, KindNames[static_cast<size_t>(kind)],
                          now.valid ? "" : "none ", now.src_stage, now.src_access, now.dst_stage,
                          now.dst_access, state.access, state.stage, old.valid ? "" : "none ",
                          old.src_stage, old.src_access, old.dst_stage, old.dst_access,
                          old_state.access, old_state.stage, n);
            }
        }
        recorded = RB::Merge(now, old);
    }
    if ((stats.requests & 0xffff) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST read_barriers: verify={} requests={} same={} elided={} broadened={} "
                 "old={} (reads: image_data={} transfer={} same_draw={} partial={}) compared={} "
                 "read_after_read_saved={} merged={} mismatches={}",
                 Vulkan::PassBreaks::g_read_barriers_verify ? 1 : 0, stats.requests,
                 stats.kinds[0], stats.kinds[1], stats.kinds[2], stats.kinds[3], stats.image_data,
                 stats.transfer, stats.same_draw, stats.partial, stats.compared,
                 stats.read_after_read, stats.merged,
                 g_read_barrier_mismatches.load(std::memory_order_relaxed));
    }

    access_mask = vk::AccessFlags2{state.access};
    stage = static_cast<vk::PipelineStageFlagBits2>(state.stage);
    // Bit 44 bookkeeping (shader_reads_visible), as GetBarrier does, from this step's barrier.
    if (!dst_reads_only) {
        shader_reads_visible = false; // the caller writes, after this barrier or none
    } else if (now.valid && (now.dst_stage & RB::AllCommands) != 0 &&
               (now.dst_access & (RB::ShaderRead | RB::MemoryRead)) != 0) {
        shader_reads_visible = true; // every write before it is visible to shader reads now
    }
    if (!recorded.valid) {
        return {};
    }
    DEBUG_ASSERT(offset < size_bytes);
    return vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlags2{recorded.src_stage},
        .srcAccessMask = vk::AccessFlags2{recorded.src_access},
        .dstStageMask = vk::PipelineStageFlags2{recorded.dst_stage},
        .dstAccessMask = vk::AccessFlags2{recorded.dst_access},
        .buffer = buffer.buffer,
        .offset = offset,
        .size = size_bytes - offset,
    };
}

void Buffer::Fill(u64 offset, u32 num_bytes, u32 value) {
    scheduler->EndRendering();
    PerfNoteWrite();
    ASSERT_MSG(offset % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer size must be a multiple of 4 bytes");
    const auto cmdbuf = scheduler->Commands();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    // FAST bit 63 (vk_transfer_eye_barriers.h): one barrier pair names both eyes' buffers.
    if (Vulkan::TransferEyeBarriers::On()) {
        if (const auto right = NativeStereo::ExistingRight(buffer)) {
            using namespace Vulkan::TransferEyeBarriers;
            if (right == buffer.buffer) {
                NoteFallback(Fallback::Alias);
            } else {
                std::array<vk::BufferMemoryBarrier2, 2> pre{pre_barrier, pre_barrier};
                std::array<vk::BufferMemoryBarrier2, 2> post{post_barrier, post_barrier};
                pre[1].buffer = right;
                post[1].buffer = right;
                // The global barrier this replaces also made earlier writes of the twin
                // available (WAW against the fill); keep that for the right buffer.
                pre[1].srcAccessMask |= vk::AccessFlagBits2::eMemoryWrite;
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                    .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                    .bufferMemoryBarrierCount = 2,
                    .pBufferMemoryBarriers = pre.data(),
                });
                cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
                cmdbuf.fillBuffer(right, offset, num_bytes, value);
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                    .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                    .bufferMemoryBarrierCount = 2,
                    .pBufferMemoryBarriers = post.data(),
                });
                NoteTaken(Site::Fill);
                return;
            }
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
    if (auto right = NativeStereo::ExistingRight(buffer)) {
        NativeStereo::Barrier(*scheduler);
        cmdbuf.fillBuffer(right, offset, num_bytes, value);
        NativeStereo::Barrier(*scheduler);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x4000;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x1000;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           MemoryUsage usage, u64 size_bytes)
    : Buffer{instance, scheduler, usage, 0, AllFlags, size_bytes,
             usage == MemoryUsage::Stream && GetStreamMemoryMode() == StreamMemoryMode::Cached} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    const auto device = instance.GetDevice();
    Vulkan::SetObjectName(device, Handle(), "StreamBuffer({}):{:#x}", BufferTypeName(usage),
                          size_bytes);
    // The shadow: only over coherent mapped memory (the copy into it needs no flush after).
    if (usage == MemoryUsage::Stream && GetStreamMemoryMode() == StreamMemoryMode::Shadow &&
        is_coherent && !mapped_data.empty()) {
        shadow.resize(size_bytes);
    }
    if (usage == MemoryUsage::Stream && std::getenv("VR_STREAM_STATS")) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        LOG_INFO(Render_Vulkan,
                 "VR stream: {} MB stream buffer in memory flags {:#x} (1 device-local, 2 "
                 "host-visible, 4 coherent, 8 cached), mode {}, shadow {}",
                 size_bytes >> 20, property_flags, static_cast<int>(GetStreamMemoryMode()),
                 !shadow.empty());
    }
}

u64 StreamBuffer::CopyReadOnlyMemo(VAddr source, size_t size, size_t alignment) {
    if (!HasShadow() || size > ScratchUpload::Memo::MaxBytes) return Copy(source, size, alignment);
    if (!upload_memo) upload_memo = std::make_unique<ScratchUpload::Memo>();
    auto& memo = *upload_memo;
    auto* memory = Core::Memory::Instance();
    const auto read_source = [&](u8* destination) {
        if (memory->IsValidMapping(source)) memory->CopySparseMemory(source, destination, size);
        else std::memcpy(destination, reinterpret_cast<const void*>(source), size);
    };
    ScratchUpload::Memo::Key key{source, scheduler->CurrentTick(), allocation_generation, alignment, size};
    std::optional<u64> found;
    if (memo.MayMatch(key)) {
        read_source(memo.snapshot.data());
        found = memo.Find(key, {memo.snapshot.data(), size});
    } else {
        ++memo.calls;
    }
    if ((memo.calls & 65535) == 0)
        LOG_INFO(Render_Vulkan, "VR exact upload memo: {} / {} hits", memo.hits, memo.calls);
    if (found) return *found;
    const auto [data, position] = Map(size, alignment);
    // Preserve Copy's sampling point: Map can wait while the guest changes memory.
    // A failed speculative comparison must not upload the pre-wait snapshot.
    read_source(data);
    Commit();
    copied_bytes += size;
    ++copied_calls;
    key.generation = allocation_generation;
    key.tick = scheduler->CurrentTick();
    memo.Store(key, {data, size}, position);
    return position;
}

void StreamBuffer::FlushShadow() {
    if (dirty_end <= dirty_begin) {
        return;
    }
    static const bool avx2_copy = std::getenv("VR_STREAM_AVX2_COPY") != nullptr;
    if (avx2_copy) {
        ScratchStream::CopyAvx2(mapped_data.data() + dirty_begin, shadow.data() + dirty_begin,
                               dirty_end - dirty_begin);
    } else {
        std::memcpy(mapped_data.data() + dirty_begin, shadow.data() + dirty_begin,
                    dirty_end - dirty_begin);
    }
    dirty_begin = ~u64{0};
    dirty_end = 0;
}

namespace {
// Keep cold option parsing out of the profiled upload function's control flow.
[[gnu::noinline]] bool RetiredUploadPreflightEnabled() {
    const char* setting = std::getenv("VR_RETIRED_UPLOAD_PREFLIGHT");
    return !setting || setting[0] != '0';
}
// Read once at start-up: Commit runs for every stream upload, and a function-local static's
// guard is a thread-local read per call.
const bool g_merge_watches = std::getenv("VR_STREAM_MERGE_WATCHES") != nullptr;
}

bool StreamBuffer::TryCopyReadOnlyBatch(std::span<const ScratchBatch::Range> ranges,
                                        std::span<u32> offsets, u64 alignment) {
    // No wrap, wait, or non-coherent atom packing changes in this experiment.
    // Consecutive original Map/Commit calls would all use the same scheduler tick.
    if (shadow.empty() || !is_coherent || size_bytes > std::numeric_limits<u32>::max())
        return false;
    u64 total;
    if (!ScratchBatch::Plan(ranges, alignment, offsets, total)) return false;
    if (!ScratchBatch::FitsWithoutWait(offset, total, alignment, size_bytes,
                                      invalidation_mark.has_value(), wait_bound)) {
        static const bool retired_preflight = RetiredUploadPreflightEnabled();
        // Check capacity first: this path must never wrap or wait. Completed watches
        // may extend the safe range beyond the last offset Map happened to visit.
        if (!retired_preflight || !invalidation_mark ||
            !ScratchBatch::FitsWithoutWait(offset, total, alignment, size_bytes, false, 0))
            return false;
        u64 start;
        if (!ScratchBatch::Aligned(offset, alignment, start) ||
            !ScratchBatch::RetiredThrough(previous_watches, wait_cursor, *invalidation_mark,
                                         wait_bound, start + total,
                                         [this](u64 tick) { return scheduler->IsFree(tick); }))
            return false;
        // Map(false) still performs the normal cursor transitions. Every watch it
        // needs was proven complete above; timeline completion is monotonic.
    }
    const auto [data, base] = Map(total, alignment, false);
    ASSERT_MSG(data != nullptr, "Preflighted upload batch could not map");
    static const bool check = std::getenv("VR_PERF_CHECK") != nullptr;
    static const bool stats = check || std::getenv("VR_PERF_STATS") != nullptr;
    thread_local u64 batches{}, buffers{}, checked{};
    auto* memory = Core::Memory::Instance();
    static const bool batch_lock = std::getenv("VR_BATCH_SPARSE_LOCK") != nullptr;
    if (batch_lock) memory->CopySparseMemoryBatch(ranges, data, offsets);
    for (size_t i=0; i<ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (!batch_lock) memory->CopySparseMemory(range.address, data+offsets[i], range.size);
        if (check) {
            thread_local std::array<u8, 16384> reference;
            ASSERT(range.size <= reference.size());
            memory->CopySparseMemory(range.address, reference.data(), range.size);
            ASSERT_MSG(std::memcmp(reference.data(), data+offsets[i], range.size) == 0,
                       "Upload batch byte mismatch at {:#x}, size {}", range.address, range.size);
            ++checked;
        }
        copied_bytes += range.size;
        ++copied_calls;
        offsets[i] += static_cast<u32>(base);
    }
    Commit();
    if (stats) {
        ++batches; buffers += ranges.size();
        if (batches % 10000 == 0)
            LOG_INFO(Render_Vulkan, "VR upload batch: {} batches / {} buffers, {} checked",
                     batches, buffers, checked);
    }
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    ScratchProfile::Cpu scratch_map(24,"StreamBuffer::Map");
    if (!is_coherent && usage == MemoryUsage::Stream) {
        size = Common::AlignUp(size, instance->NonCoherentAtomSize());
    }

    if (size > this->size_bytes) {
        return {nullptr, 0};
    }

    mapped_size = size;

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        ++allocation_generation;
        // The ring's tail goes into the BAR before the ring starts over (with a shadow).
        FlushShadow();
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    const u64 mapped_upper_bound = offset + size;
    if (!WaitPendingOperations(mapped_upper_bound, allow_wait)) {
        return {nullptr, 0};
    }

    if (!shadow.empty()) {
        // Marked here, not in Commit: some callers write a mapped range without committing it.
        dirty_begin = std::min(dirty_begin, offset);
        dirty_end = std::max(dirty_end, mapped_upper_bound);
        return {shadow.data() + offset, offset};
    }
    return {mapped_data.data() + offset, offset};
}

void StreamBuffer::Commit() {
    ScratchProfile::Cpu scratch_commit(25,"StreamBuffer::Commit");
    if (!is_coherent) {
        if (usage == MemoryUsage::Download) {
            vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset,
                                    mapped_size);
        } else {
            vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, mapped_size);
        }
    }

    offset += mapped_size;
    ScratchStream::CommitWatch(current_watches, current_watch_cursor,
                               scheduler->CurrentTick(), offset, g_merge_watches,
                               WATCHES_RESERVE_CHUNK);
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler->IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler->Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
