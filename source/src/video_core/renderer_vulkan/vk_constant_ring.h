// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// B4: A-owned constant allocations, B-published GPU retirement. Opt-in only.
#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include "common/alignment.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
namespace Vulkan {
class ConstantRing : public VideoCore::Buffer {
public:
    static constexpr u64 PageBytes = 4 * 1024 * 1024;
    static constexpr u32 Pages = 8;
    static constexpr u32 NoPage = ~u32{0};
    ConstantRing(const Instance& instance, Scheduler& scheduler)
        : Buffer(instance, scheduler, VideoCore::MemoryUsage::Upload, 0,
                 vk::BufferUsageFlagBits::eStorageBuffer, Pages * PageBytes, true) {
        ASSERT(mapped_data.size() == Pages * PageBytes);
    }
    // Reserve one page for a complete packet. A never wraps storage belonging to
    // an unpublished packet. A's ticket is published after queue insertion; B's
    // tick is published after recording all uses (including mid-draw submissions).
    template <class Pending, class Drain>
    bool BeginPacket(u64 maximum, Pending pending, Drain drain) {
        if (maximum > PageBytes)
            return false;
        if (offset + maximum > (page + 1) * PageBytes) {
            page = (page + 1) % Pages;
            if (pending(tickets[page]))
                drain();
            const auto tick = ticks[page].load(std::memory_order_acquire);
            if (tick && !scheduler->IsFree(tick)) {
                drain(); // Scheduler producer ownership belongs to A only after this.
                scheduler->Wait(tick);
            }
            offset = page * PageBytes;
            ++generation;
        }
        limit = offset + maximum;
        return true;
    }
    std::pair<u8*, u64> Map(u64 bytes, u64 alignment = 0) {
        const auto atom = is_coherent ? 1 : instance->NonCoherentAtomSize();
        alignment = std::max<u64>(alignment, atom);
        const auto start = Common::AlignUp(offset, alignment);
        ASSERT(start + bytes <= limit);
        offset = start + Common::AlignUp(bytes, atom);
        commit_begin = start;
        return {mapped_data.data() + start, start};
    }
    void Commit() {
        if (!is_coherent)
            vmaFlushAllocation(buffer.allocator, buffer.allocation, commit_begin,
                               offset - commit_begin);
    }
    u64 Copy(const void* source, size_t bytes, size_t alignment = 0) {
        const auto [target, at] = Map(bytes, alignment);
        std::memcpy(target, source, bytes);
        Commit();
        return at;
    }
    u64 AllocationGeneration() const {
        return generation;
    }
    u32 Page() const {
        return page;
    }
    void Published(u64 ticket) {
        tickets[page] = ticket;
    }
    void Used(u32 used_page, u64 tick) {
        if (used_page != NoPage)
            ticks[used_page].store(tick, std::memory_order_release);
    }

private:
    // Cache lines by writer (bbport vk_constant_ring.h:113-114): B stores a tick for every packet
    // (Used); A writes the tickets and the cursor for every packet and reads the Buffer fields
    // above (mapped_data, perf_serial).
    alignas(64) std::array<std::atomic<u64>, Pages> ticks{};
    alignas(64) std::array<u64, Pages> tickets{};
    u64 offset{}, limit{}, commit_begin{}, generation{1};
    u32 page{};
};
} // namespace Vulkan
