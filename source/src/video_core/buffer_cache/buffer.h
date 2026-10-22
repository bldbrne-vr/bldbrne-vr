// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/native_stereo/exact_upload_memo.h"
#include "video_core/native_stereo/upload_batch.h"
#include <memory>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
#include "common/types.h"
#include "core/memory.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pass_breaks.h"
#include "video_core/renderer_vulkan/vr_perf.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

struct VmaAllocationInfo;

namespace NativeStereo {
struct BufferEntry;
}

namespace VideoCore {

/// Hints and requirements for the backing memory type of a commit
enum class MemoryUsage {
    DeviceLocal, ///< Requests device local buffer.
    Upload,      ///< Requires a host visible memory type optimized for CPU to GPU uploads
    Download,    ///< Requires a host visible memory type optimized for GPU to CPU readbacks
    Stream,      ///< Requests device local host visible buffer, falling back host memory.
};

constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;

/// FastRender bit 44 (Buffer::shader_reads_visible): the accesses Buffer::GetBarrier counts as
/// reads. Any other bit in a requested mask counts as a write.
constexpr vk::AccessFlags2 BarrierReadAccess =
    vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eIndexRead |
    vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eUniformRead |
    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead |
    vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eMemoryRead;

struct UniqueBuffer {
    explicit UniqueBuffer(vk::Device device, VmaAllocator allocator);
    ~UniqueBuffer();

    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer& operator=(const UniqueBuffer&) = delete;

    UniqueBuffer(UniqueBuffer&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          buffer{std::exchange(other.buffer, VK_NULL_HANDLE)},
          native_entry{std::exchange(other.native_entry, nullptr)} {}
    UniqueBuffer& operator=(UniqueBuffer&& other) {
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        native_entry = std::exchange(other.native_entry, nullptr);
        return *this;
    }

    void Create(const vk::BufferCreateInfo& image_ci, MemoryUsage usage,
                VmaAllocationInfo* out_alloc_info, bool host_cached = false);

    operator vk::Buffer() const {
        return buffer;
    }

    vk::Device device;
    VmaAllocator allocator;
    VmaAllocation allocation;
    vk::Buffer buffer{};
    vk::DeviceAddress bda_addr = 0;
    // Native stereo registry entry of `buffer` (null when not registered); lives as long as
    // `buffer`, so bindings carry it instead of looking the right eye up (fast bit 25).
    NativeStereo::BufferEntry* native_entry{};
};

class Buffer {
public:
    explicit Buffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                    MemoryUsage usage, VAddr cpu_addr_, vk::BufferUsageFlags flags,
                    u64 size_bytes_, bool host_cached = false);

    Buffer& operator=(const Buffer&) = delete;
    Buffer(const Buffer&) = delete;

    Buffer& operator=(Buffer&&) = default;
    Buffer(Buffer&&) = default;

    void IncreaseStreamScore(int score) noexcept {
        stream_score += score;
    }

    [[nodiscard]] int StreamScore() const noexcept {
        return stream_score;
    }

    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        return addr >= cpu_addr && addr + size <= cpu_addr + SizeBytes();
    }

    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return cpu_addr;
    }

    [[nodiscard]] u64 Offset(VAddr other_cpu_addr) const noexcept {
        return other_cpu_addr - cpu_addr;
    }

    size_t SizeBytes() const {
        return size_bytes;
    }

    void SetLRUId(u64 id) noexcept {
        lru_id = id;
    }

    u64 LRUId() const noexcept {
        return lru_id;
    }

    vk::Buffer Handle() const noexcept {
        return buffer;
    }

    NativeStereo::BufferEntry* NativeEntry() const noexcept {
        return buffer.native_entry;
    }

    vk::DeviceAddress BufferDeviceAddress() const noexcept {
        ASSERT_MSG(buffer.bda_addr != 0, "Can't get BDA from a non BDA buffer");
        return buffer.bda_addr;
    }

    std::optional<vk::BufferMemoryBarrier2> GetBarrier(vk::AccessFlags2 dst_acess_mask,
                                                       vk::PipelineStageFlagBits2 dst_stage,
                                                       u32 offset = 0) {
        if (Vulkan::PassBreaks::g_read_barriers) [[unlikely]] {
            // The same read again (most requests): no barrier and no change, as with the switch
            // off. The old step's own state is the same one then (read_barrier_logic.h), so
            // FAST_GPU_READ_BARRIERS_VERIFY has nothing to compare either.
            if (dst_acess_mask == access_mask && stage == dst_stage &&
                (dst_acess_mask & BarrierReadAccess) == dst_acess_mask) {
                return {};
            }
            return GetBarrierReads(dst_acess_mask, dst_stage, offset);
        }
        // Bit 44 bookkeeping (shader_reads_visible) only: no barrier here depends on it.
        const bool dst_reads_only = (dst_acess_mask & BarrierReadAccess) == dst_acess_mask;
        if (dst_acess_mask == access_mask && stage == dst_stage) {
            if (!dst_reads_only) {
                shader_reads_visible = false; // the caller writes, after no barrier
            }
            return {};
        }

        DEBUG_ASSERT(offset < size_bytes);

        const auto barrier = vk::BufferMemoryBarrier2{
            .srcStageMask = stage,
            .srcAccessMask = access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_acess_mask,
            .buffer = buffer.buffer,
            .offset = offset,
            .size = size_bytes - offset,
        };
        if (Vulkan::PassBreaks::g_stats_enabled && Vulkan::PassBreaks::IsPureRead(access_mask) &&
            Vulkan::PassBreaks::IsPureRead(dst_acess_mask)) [[unlikely]] {
            // FAST_PASS_BREAK_STATS: what FAST_GPU_READ_BARRIERS would save.
            Vulkan::PassBreaks::Count(Vulkan::PassBreaks::g_barriers.read_after_read);
        }
        access_mask = dst_acess_mask;
        stage = dst_stage;
        if (!dst_reads_only) {
            shader_reads_visible = false; // the caller writes after this barrier
        } else if (dst_stage == vk::PipelineStageFlagBits2::eAllCommands &&
                   (dst_acess_mask &
                    (vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eMemoryRead))) {
            shader_reads_visible = true; // every write before it is visible to shader reads now
        }
        return barrier;
    }

    /// FAST_GPU_READ_BARRIERS (vk_pass_breaks.h, read_barrier_logic.h): GetBarrier with the
    /// switch on (buffer.cpp).
    std::optional<vk::BufferMemoryBarrier2> GetBarrierReads(vk::AccessFlags2 dst_acess_mask,
                                                            vk::PipelineStageFlagBits2 dst_stage,
                                                            u32 offset);

    /// FAST_GPU_READ_BARRIERS: every write so far is visible to every read at every stage and no
    /// write was asked for since, so a read of any kind asks for no barrier (GetBarrierReads; a
    /// buffer holding image data takes the old step).
    [[nodiscard]] bool ReadsSettled() const noexcept {
        return Vulkan::PassBreaks::g_read_barriers && !image_data &&
               stage == vk::PipelineStageFlagBits2::eAllCommands &&
               access_mask == Vulkan::PassBreaks::AnyRead;
    }

    /// Bit 44 (BufferCache::ObtainSmallDirect): the state BindBuffers' shader-read GetBarrier
    /// would leave, set without its barrier (FAST_GPU_READ_BARRIERS_VERIFY's old state as well).
    void SettleShaderRead() noexcept {
        access_mask = vk::AccessFlagBits2::eShaderRead;
        stage = vk::PipelineStageFlagBits2::eAllCommands;
        if (Vulkan::PassBreaks::g_read_barriers_verify) {
            verify_access_mask = access_mask;
            verify_stage = vk::PipelineStageFlagBits2::eAllCommands;
        }
    }

    void Fill(u64 offset, u32 num_bytes, u32 value);

public:
    VAddr cpu_addr = 0;
    bool is_picked{};
    bool is_coherent{};
    bool is_deleted{};
    int stream_score = 0;
    size_t size_bytes = 0;
    u64 lru_id = 0;
    std::span<u8> mapped_data;
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    MemoryUsage usage;
    UniqueBuffer buffer;
    vk::Flags<vk::AccessFlagBits2> access_mask{
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
    vk::PipelineStageFlagBits2 stage{vk::PipelineStageFlagBits2::eAllCommands};
    // GPU-pass optimizations (vr_perf.h, the resync memo): this buffer's identity (never reused),
    // its write counter (uploads, shader / copy / fill writes, resyncs into it) and the draw whose
    // bindings last bound it for writing.
    u64 perf_serial = Vulkan::Perf::NextBufferSerial();
    u64 perf_write_seq{1};
    u64 perf_bound_draw{0};
    // FAST_GPU_READ_BARRIERS (GetBarrierReads): the draw (Perf::DrawSerial) whose bindings last
    // asked for a write access; a read asked for by the same draw takes the old step (the write
    // lands after this draw's barriers).
    u64 write_request_draw{0};
    // FAST_GPU_READ_BARRIERS_VERIFY: the state the switch-off GetBarrier would be in.
    vk::AccessFlags2 verify_access_mask{
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
    vk::PipelineStageFlags2 verify_stage{vk::PipelineStageFlagBits2::eAllCommands};
    // FastRender bit 33 (BufferCache::ObtainSmallDirect): a resync wrote image data into this
    // buffer (SynchronizeBufferFromImage, inherited through JoinOverlap). Its bytes may then
    // differ from guest memory on pages the tracker sees CPU-clean, so no small read-only range
    // is bound from it in place. Never cleared.
    bool image_data{};
    // FastRender bit 44 (BufferCache::ObtainSmallDirect): every write recorded into this buffer so
    // far is visible to shader reads at every stage. GetBarrier clears it when a write access is
    // asked for (with or without a barrier) and sets it with a barrier into a shader or memory
    // read at every stage; JoinOverlap sets it after its copy's barrier. The writes no GetBarrier
    // sees end with their own barrier into memory read at every stage (SynchronizeBuffer's
    // upload, WriteDataBuffer, Fill, CopyBuffer, the copy-shader HLE) or mark the buffer
    // image_data (never bound in place). A new buffer holds no write.
    bool shader_reads_visible{true};

    /// A write into this buffer is recorded now.
    void PerfNoteWrite() {
        ++perf_write_seq;
    }
    /// This buffer is bound for writing (a shader writes it with the current draw, after its
    /// bindings; a copy or a fill right away).
    void PerfNoteWriteBound() {
        ++perf_write_seq;
        perf_bound_draw = Vulkan::Perf::DrawSerial();
    }
};

/// Where the stream buffer keeps its data (buffer.cpp, VR_STREAM_MEM).
enum class StreamMemoryMode : int { Device = 0, Cached = 2, Shadow = 3, Default = Shadow };
StreamMemoryMode GetStreamMemoryMode();

class StreamBuffer : public Buffer {
public:
    explicit StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                          MemoryUsage usage, u64 size_bytes_);

    // Changes before ring offsets can be recycled; used by scratch stereo upload reuse.
    u64 AllocationGeneration() const noexcept { return allocation_generation; }

    /// Reserves a region of memory from the stream buffer.
    std::pair<u8*, u64> Map(u64 size, u64 alignment = 0, bool allow_wait = true);

    /// Ensures that reserved bytes of memory are available to the GPU.
    void Commit();

    /// With a shadow (StreamMemoryMode::Shadow): copies what was written since the last flush
    /// into the GPU's memory. The scheduler calls it before every submit.
    void FlushShadow();

    [[nodiscard]] bool HasShadow() const noexcept {
        return !shadow.empty();
    }

    u64 CopyReadOnlyMemo(VAddr source, size_t size, size_t alignment);

    bool TryCopyReadOnlyBatch(std::span<const ScratchBatch::Range> ranges,
                             std::span<u32> offsets, u64 alignment);


    /// Maps and commits a memory region with user provided data
    u64 Copy(auto src, size_t size, size_t alignment = 0) {
        copied_bytes += size;
        ++copied_calls;
        const auto [data, offset] = Map(size, alignment);
        auto* memory = Core::Memory::Instance();
        const VAddr src_vaddr = reinterpret_cast<const VAddr>(src);
        if (memory->IsValidMapping(src_vaddr)) {
            memory->CopySparseMemory(src_vaddr, data, size);
        } else {
            std::memcpy(data, reinterpret_cast<const void*>(src), size);
        }
        Commit();
        return offset;
    }

private:
    struct Watch {
        u64 tick{};
        u64 upper_bound{};
    };

    /// Increases the amount of watches available.
    void ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size);

    /// Waits pending watches until requested upper bound.
    bool WaitPendingOperations(u64 requested_upper_bound, bool allow_wait);

public:
    /// Bytes and calls through Copy since the last call (VR_STREAM_STATS).
    std::pair<u64, u64> TakeCopyStats() {
        const std::pair<u64, u64> r{copied_bytes, copied_calls};
        copied_bytes = copied_calls = 0;
        return r;
    }

private:
    std::unique_ptr<ScratchUpload::Memo> upload_memo;
    u64 allocation_generation{};
    std::vector<u8> shadow;
    u64 dirty_begin{~u64{0}};
    u64 dirty_end{};
    u64 copied_bytes{};
    u64 copied_calls{};
    u64 offset{};
    u64 mapped_size{};
    std::vector<Watch> current_watches;
    std::size_t current_watch_cursor{};
    std::optional<size_t> invalidation_mark;
    std::vector<Watch> previous_watches;
    std::size_t wait_cursor{};
    u64 wait_bound{};
};

} // namespace VideoCore
