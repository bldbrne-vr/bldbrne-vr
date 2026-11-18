// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/upload_staging.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {
namespace {
constexpr u64 BlockSize = 16_MB;
constexpr u64 PrewarmBytes = 128_MB;
constexpr u64 KeepBytes = 512_MB;
// VR_UPLOAD_POISON=1 (test only): a block's bytes are overwritten with a pattern whenever it is
// rewound for reuse, so a GPU read of an old upload that had not completed would see garbage
// (broken textures or geometry) instead of the right bytes by luck.
const bool g_poison = [] {
    const char* value = std::getenv("VR_UPLOAD_POISON");
    return value && value[0] == '1';
}();
void Poison(Buffer& buffer, u64 capacity) {
    std::memset(buffer.mapped_data.data(), 0xFD, capacity);
    if (!buffer.is_coherent) {
        vmaFlushAllocation(buffer.instance->GetAllocator(), buffer.buffer.allocation, 0, capacity);
    }
}
} // namespace

void UploadStagingPool::Allocation::Flush() const {
    if (!buffer->is_coherent) {
        const auto result = vmaFlushAllocation(buffer->instance->GetAllocator(),
                                               buffer->buffer.allocation, offset, size);
        ASSERT_MSG(result == VK_SUCCESS, "Failed flushing upload staging memory");
    }
}

void UploadStagingPool::Allocation::Retire() const {
    if (state) {
        ASSERT(state->reservations != 0);
        state->Commit(buffer->scheduler->CurrentTick());
    }
}

UploadStagingPool::UploadStagingPool(const Vulkan::Instance& instance_,
                                     Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, stats{std::getenv("VR_UPLOAD_STATS") != nullptr} {
    // bbport's Windows pool populates 128 MiB at startup to move OS first-touch
    // faults out of the first streaming burst. Grow beyond this only on demand.
    for (u64 bytes = 0; bytes < PrewarmBytes; bytes += BlockSize) {
        auto& block = AddBlock(BlockSize);
        std::memset(block.buffer->mapped_data.data(), 0, BlockSize);
        Allocation{block.buffer.get(), block.buffer->mapped_data.data(), 0, BlockSize}.Flush();
    }
}

UploadStagingPool::Block& UploadStagingPool::AddBlock(u64 size) {
    auto buffer =
        std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0, AllFlags, size);
    ASSERT_MSG(buffer->mapped_data.size() >= size, "Upload staging memory is not mapped");
    blocks.push_back(std::make_unique<Block>(Block{std::move(buffer), {.capacity = size}}));
    allocated_bytes += size;
    peak_bytes = std::max(peak_bytes, allocated_bytes);
    ++allocations;
    return *blocks.back();
}

UploadStagingPool::Allocation UploadStagingPool::Request(u64 size, u64 alignment) {
    ASSERT(size > 0 && size <= std::numeric_limits<u64>::max() - (64_KB - 1));
    alignment = std::max(alignment, u64{4});
    ASSERT((alignment & (alignment - 1)) == 0);
    // Only non-coherent allocations need separate flush atoms. Coherence can
    // differ between blocks if VMA falls back to another memory type.
    const auto alignment_for = [&](const Buffer& buffer) {
        return buffer.is_coherent ? alignment
                                  : std::max(alignment, u64{instance.NonCoherentAtomSize()});
    };
    const u64 tick = scheduler.CurrentTick();
    const bool large = size > BlockSize;
    const u64 capacity = large ? Common::AlignUp(size, u64{64_KB}) : BlockSize;
    ++requests;
    upload_bytes += size;

    std::optional<size_t> best;
    for (size_t n = 0; n < blocks.size(); ++n) {
        const size_t index = (current + n) % blocks.size();
        auto& block = *blocks[index];
        auto& state = block.state;
        if (large) {
            // Best fit avoids consuming a bigger reusable buffer and then needing
            // another allocation for the next, larger upload in the same burst.
            if (state.capacity >= capacity && state.capacity - capacity <= capacity / 4 &&
                state.reservations == 0 && state.tick < tick &&
                (!best || state.capacity < blocks[*best]->state.capacity) &&
                scheduler.IsFree(state.tick)) {
                best = index;
            }
            continue;
        }
        if (state.capacity != BlockSize) {
            continue;
        }
        const u64 block_alignment = alignment_for(*block.buffer);
        auto offset = state.Append(size, block_alignment, tick, frame);
        // CurrentTick has not been submitted. IsFree would refresh the Vulkan
        // semaphore for every full block even though none can be reusable yet.
        if (!offset && state.reservations == 0 && state.tick < tick &&
            state.Rewind(scheduler.IsFree(state.tick))) {
            if (g_poison) {
                Poison(*block.buffer, state.capacity);
            }
            offset = state.Append(size, block_alignment, tick, frame);
        }
        if (offset) {
            current = index;
            return {block.buffer.get(), block.buffer->mapped_data.data() + *offset, *offset, size,
                    &block.state};
        }
    }

    // Never wait for the recorder/GPU here: Request may hold protected source
    // region locks. An in-flight allocation stays alive even above KeepBytes.
    auto& block = best ? *blocks[*best] : AddBlock(capacity);
    current = best.value_or(blocks.size() - 1);
    if (best) {
        // Completion was established above and is monotonic.
        const bool rewound = block.state.Rewind(true);
        ASSERT(rewound);
        if (g_poison) {
            Poison(*block.buffer, block.state.capacity);
        }
    }
    const auto offset = block.state.Append(size, alignment_for(*block.buffer), tick, frame);
    ASSERT(offset.has_value());
    return {block.buffer.get(), block.buffer->mapped_data.data() + *offset, *offset, size,
            &block.state};
}

void UploadStagingPool::TickFrame() {
    ++frame;
    const u64 tick = scheduler.CurrentTick();
    // Retain the working set across loads; trim at most one retired allocation
    // per frame so a burst does not turn into a burst of driver frees later.
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& state = blocks[i]->state;
        if ((allocated_bytes > KeepBytes || state.capacity > BlockSize) &&
            state.CanTrim(frame, true) && state.tick < tick && scheduler.IsFree(state.tick)) {
            allocated_bytes -= state.capacity;
            blocks.erase(blocks.begin() + i);
            current = 0;
            break;
        }
    }
    if (stats && frame % 300 == 0) {
        LOG_INFO(Render_Vulkan,
                 "Upload staging: {} requests, {} MiB copied, {} allocations since startup, "
                 "{} blocks / {} MiB resident, {} MiB peak",
                 requests, upload_bytes >> 20, allocations, blocks.size(), allocated_bytes >> 20,
                 peak_bytes >> 20);
    }
}

} // namespace VideoCore
