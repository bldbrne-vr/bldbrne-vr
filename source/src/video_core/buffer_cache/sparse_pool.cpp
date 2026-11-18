// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <functional>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/slow_scope.h"
#include "video_core/buffer_cache/sparse_bind_batch.h"
#include "video_core/buffer_cache/sparse_pool.h"
#include "video_core/renderer_vulkan/vk_gpu_crash.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace VideoCore {

namespace L = SparseLogic;

SparsePool::SparsePool(const Vulkan::Instance& instance_)
    : instance{instance_}, book{[this] { return TakePiece(); },
                                [this](u32 piece) { GivePiece(piece); }} {
    if (!instance.SupportsSparseBuffers()) {
        return;
    }
    const auto device = instance.GetDevice();
    // The block and the memory types a sparse buffer takes (a probe buffer's requirements).
    const vk::BufferCreateInfo probe_ci = {
        .flags = vk::BufferCreateFlagBits::eSparseBinding |
                 vk::BufferCreateFlagBits::eSparseResidency |
                 vk::BufferCreateFlagBits::eSparseAliased,
        .size = L::SlabSize,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {.pCreateInfo = &probe_ci};
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    if (reqs.alignment == 0 || L::BlockSize % reqs.alignment != 0) {
        LOG_WARNING(Render_Vulkan, "Sparse buffers: block {} KB does not divide 64 KB: off",
                    reqs.alignment >> 10);
        return;
    }
    const auto& props = instance.GetPhysicalDevice().getMemoryProperties();
    std::optional<u32> type;
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if (((reqs.memoryTypeBits >> i) & 1) != 0 &&
            (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            type = i;
            break;
        }
    }
    if (!type) {
        LOG_WARNING(Render_Vulkan, "Sparse buffers: no device-local memory type: off");
        return;
    }
    memory_type = *type;
    const vk::SemaphoreTypeCreateInfo timeline_ci = {
        .semaphoreType = vk::SemaphoreType::eTimeline,
        .initialValue = 0,
    };
    auto [result, semaphore] = device.createSemaphoreUnique({.pNext = &timeline_ci});
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Sparse buffers: no timeline semaphore: off");
        return;
    }
    timeline = std::move(semaphore);
    enabled = true;
    LOG_INFO(Render_Vulkan,
             "Sparse buffers: ON (block {} KB, slabs of {} MB, memory type {}): the cache's "
             "buffers are views of shared pieces; merges bind instead of copying",
             reqs.alignment >> 10, L::SlabSize >> 20, memory_type);
}

SparsePool::~SparsePool() {
    const auto device = instance.GetDevice();
    for (const Slab& slab : slabs) {
        if (slab.memory) {
            device.freeMemory(slab.memory);
        }
    }
}

std::optional<u32> SparsePool::TakePiece() {
    // From the lowest slab that may have room (first_room): the slabs below it are full or
    // freed, so a load taking thousands of pieces does not walk them again for each.
    for (u32 i = first_room; i < slabs.size(); ++i) {
        Slab& slab = slabs[i];
        if (!slab.memory || slab.used == L::PiecesPerSlab) {
            if (i == first_room) {
                ++first_room;
            }
            continue;
        }
        for (u32 word = 0; word < slab.free.size(); ++word) {
            if (slab.free[word] != 0) {
                const u32 bit = static_cast<u32>(std::countr_zero(slab.free[word]));
                slab.free[word] &= slab.free[word] - 1;
                ++slab.used;
                slab.empty_since_ns = 0;
                ++resident_pieces;
                return L::Piece(i, word * 64 + bit);
            }
        }
    }
    // FAST_SPARSE_MAX_MB=N (a test, read once): slabs past N MiB fail as on a full card, so the
    // fallbacks run (ordinary buffers for views, a write to both eyes where a twin's blocks
    // could not be made its own).
    static const u64 max_slabs = [] {
        const char* value = std::getenv("FAST_SPARSE_MAX_MB");
        return value ? (u64(std::strtoull(value, nullptr, 10)) << 20) / L::SlabSize : ~u64{0};
    }();
    u64 live_slabs = 0;
    for (const Slab& slab : slabs) {
        live_slabs += slab.memory ? 1 : 0;
    }
    if (live_slabs >= max_slabs) {
        return std::nullopt;
    }
    const vk::MemoryAllocateFlagsInfo flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    Common::Slow::Scope slow{Common::Slow::SlabAlloc, L::SlabSize};
    const auto [result, memory] = instance.GetDevice().allocateMemory({
        .pNext = &flags,
        .allocationSize = L::SlabSize,
        .memoryTypeIndex = memory_type,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Sparse buffers: no {} MB slab ({}), {} MiB in pieces",
                    L::SlabSize >> 20, vk::to_string(result),
                    (resident_pieces * L::BlockSize) >> 20); // under the lock already
        return std::nullopt;
    }
    // A slot a freed slab left, or a new one.
    u32 slab_index = static_cast<u32>(slabs.size());
    for (u32 i = 0; i < slabs.size(); ++i) {
        if (!slabs[i].memory) {
            slab_index = i;
            break;
        }
    }
    if (slab_index == slabs.size()) {
        slabs.emplace_back();
    }
    first_room = std::min(first_room, slab_index);
    Slab& slab = slabs[slab_index];
    slab = {.memory = memory};
    slab.free.fill(~u64{0});
    slab.free[0] &= ~u64{1}; // piece 0 taken now
    slab.used = 1;
    ++resident_pieces;
    return L::Piece(slab_index, 0);
}

void SparsePool::GivePiece(u32 piece) {
    first_room = std::min(first_room, L::SlabOf(piece));
    Slab& slab = slabs[L::SlabOf(piece)];
    const u32 index = piece % L::PiecesPerSlab;
    slab.free[index / 64] |= u64{1} << (index % 64);
    --slab.used;
    --resident_pieces;
}

std::unique_lock<std::mutex> SparsePool::Lock() {
    std::unique_lock lock{mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        Common::Slow::Scope slow{Common::Slow::PoolWait};
        lock.lock();
    }
    return lock;
}

vk::Buffer SparsePool::CreateView(VAddr begin, u64 size, vk::BufferUsageFlags usage,
                                  std::vector<u32>& pieces) {
    ASSERT(enabled && begin % L::BlockSize == 0 && size % L::BlockSize == 0 && size != 0);
    const vk::BufferCreateInfo buffer_ci = {
        .flags = vk::BufferCreateFlagBits::eSparseBinding |
                 vk::BufferCreateFlagBits::eSparseResidency |
                 vk::BufferCreateFlagBits::eSparseAliased,
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [result, buffer] = instance.GetDevice().createBuffer(buffer_ci);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    const auto lock = Lock();
    auto bound = book.BindView(begin >> L::BlockBits, size >> L::BlockBits);
    if (!bound) {
        // VRAM full: nothing taken (Book undoes the call); an ordinary buffer is made instead.
        instance.GetDevice().destroyBuffer(buffer);
        return {};
    }
    pieces = std::move(*bound);
    QueueBinds(buffer, pieces);
    return buffer;
}

void SparsePool::ReleaseView(VAddr begin, const std::vector<u32>& pieces) {
    std::scoped_lock lock{mutex};
    book.ReleaseView(begin >> L::BlockBits, pieces);
}

vk::Buffer SparsePool::CreateTwin(VAddr begin, u64 size, vk::BufferUsageFlags usage,
                                  std::vector<u64>& private_bits, std::vector<u32>& pieces) {
    ASSERT(enabled && begin % L::BlockSize == 0 && size % L::BlockSize == 0 && size != 0);
    const vk::BufferCreateInfo buffer_ci = {
        .flags = vk::BufferCreateFlagBits::eSparseBinding |
                 vk::BufferCreateFlagBits::eSparseResidency |
                 vk::BufferCreateFlagBits::eSparseAliased,
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [result, buffer] = instance.GetDevice().createBuffer(buffer_ci);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    const auto lock = Lock();
    pieces = book.BindTwin(begin >> L::BlockBits, size >> L::BlockBits, private_bits);
    QueueBinds(buffer, pieces);
    return buffer;
}

bool SparsePool::Privatize(VAddr view_begin, VAddr begin, u64 size, std::vector<u64>& fresh) {
    const u64 view_block = view_begin >> L::BlockBits;
    const u64 first_block = L::AlignDown(begin) >> L::BlockBits;
    const u64 end_block = L::AlignUp(begin + size) >> L::BlockBits;
    std::vector<u64> blocks_taken;
    const auto lock = Lock();
    if (!book.Privatize(first_block, end_block - first_block, blocks_taken)) {
        return false; // all or nothing: no uninitialized right piece stays
    }
    for (const u64 b : blocks_taken) {
        fresh.push_back(b - view_block);
    }
    return true;
}

void SparsePool::Unprivatize(VAddr view_begin, const std::vector<u64>& fresh) {
    const u64 view_block = view_begin >> L::BlockBits;
    std::vector<u64> blocks_taken;
    blocks_taken.reserve(fresh.size());
    for (const u64 b : fresh) {
        blocks_taken.push_back(b + view_block);
    }
    std::scoped_lock lock{mutex};
    book.Unprivatize(blocks_taken);
}

void SparsePool::ReleaseTwin(const std::vector<u32>& pieces) {
    std::scoped_lock lock{mutex};
    book.Release(pieces);
}

void SparsePool::QueueBinds(vk::Buffer buffer, const std::vector<u32>& pieces) {
    const auto runs = L::Runs(pieces);
    std::vector<vk::SparseMemoryBind> binds;
    binds.reserve(runs.size());
    for (const L::Run& run : runs) {
        binds.push_back({
            .resourceOffset = run.first << L::BlockBits,
            .size = run.blocks << L::BlockBits,
            .memory = slabs[L::SlabOf(run.piece)].memory,
            .memoryOffset = L::OffsetIn(run.piece),
        });
    }
    pending.emplace_back(buffer, std::move(binds));
}

u64 SparsePool::SubmitBinds(vk::Queue queue) {
    std::scoped_lock lock{mutex};
    if (pending.empty()) {
        // Nothing new: wait for the newest bind only until it is known done (then never again).
        if (timeline_value == known_done) {
            return 0;
        }
        const auto [result, done] = instance.GetDevice().getSemaphoreCounterValue(*timeline);
        if (result == vk::Result::eSuccess && done >= timeline_value) {
            known_done = timeline_value;
            return 0;
        }
        return timeline_value;
    }
    // Reused between submissions: a load sends hundreds of views' binds.
    thread_local std::vector<vk::SparseBufferMemoryBindInfo> infos;
    infos.clear();
    infos.reserve(pending.size());
    for (const auto& [buffer, binds] : pending) {
        infos.push_back({
            .buffer = buffer,
            .bindCount = static_cast<u32>(binds.size()),
            .pBinds = binds.data(),
        });
    }
    const SparseBindBatch batch{*timeline, timeline_value, infos};
    const u64 value = batch.signal_value;
    const vk::Result result = [&] {
        Common::Slow::Scope slow{Common::Slow::SparseBind, infos.size()};
        return queue.bindSparse(batch.info);
    }();
    Vulkan::GpuCrash::Report(result, fmt::format("the sparse binds of value {}", value));
    ASSERT_MSG(result == vk::Result::eSuccess, "Sparse buffers: vkQueueBindSparse failed ({})",
               vk::to_string(result));
    timeline_value = value;
    Vulkan::GpuCrash::NoteSparseBind(static_cast<u32>(infos.size()), value);
    if constexpr (Common::Log::Enabled) {
        static u64 submits = 0, binds = 0;
        ++submits;
        for (const auto& info : infos) {
            binds += info.bindCount;
        }
        if ((submits & (submits - 1)) == 0 && submits >= 16) {
            LOG_INFO(Render_Vulkan, "Sparse binds: {} submissions, {} binds so far", submits, binds);
        }
    }
    pending.clear();
    return timeline_value;
}

void SparsePool::Collect(u64 now_ns) {
    // A slab empty for 3 s goes back (the views that bound it were destroyed before their pieces
    // came back: ReleaseView runs after the buffer's erase). One empty slab stays as a spare.
    static constexpr s64 EmptyFor = 3'000'000'000;
    std::scoped_lock lock{mutex};
    bool spare = false;
    for (u32 i = 0; i < slabs.size(); ++i) {
        Slab& slab = slabs[i];
        if (!slab.memory || slab.used != 0) {
            continue;
        }
        if (slab.empty_since_ns == 0) {
            slab.empty_since_ns = static_cast<s64>(now_ns);
            continue;
        }
        if (!spare) {
            spare = true;
            continue;
        }
        if (static_cast<s64>(now_ns) - slab.empty_since_ns < EmptyFor) {
            continue;
        }
        instance.GetDevice().freeMemory(slab.memory);
        slab = {};
    }
}

} // namespace VideoCore
