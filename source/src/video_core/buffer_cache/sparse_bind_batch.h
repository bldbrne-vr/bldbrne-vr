// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

// Owns the small arrays/pNext data consumed synchronously by vkQueueBindSparse.
// Non-movable because the Vulkan structures point into this object.
// Sparse batches have no implicit ordering, even on one queue. Chain each batch
// to its predecessor so a wait for N also covers all bindings in batches < N.
// https://docs.vulkan.org/spec/latest/chapters/sparsemem.html#sparsememory-memory-binding
struct SparseBindBatch {
    SparseBindBatch(vk::Semaphore timeline_semaphore, std::uint64_t previous,
                    std::span<const vk::SparseBufferMemoryBindInfo> buffers)
        : wait_value{previous}, signal_value{previous + 1}, semaphore{timeline_semaphore},
          timeline{.waitSemaphoreValueCount = previous != 0 ? 1u : 0u,
                   .pWaitSemaphoreValues = previous != 0 ? &wait_value : nullptr,
                   .signalSemaphoreValueCount = 1,
                   .pSignalSemaphoreValues = &signal_value},
          info{.pNext = &timeline,
               .waitSemaphoreCount = timeline.waitSemaphoreValueCount,
               .pWaitSemaphores = previous != 0 ? &semaphore : nullptr,
               .bufferBindCount = static_cast<std::uint32_t>(buffers.size()),
               .pBufferBinds = buffers.data(),
               .signalSemaphoreCount = 1,
               .pSignalSemaphores = &semaphore} {}

    SparseBindBatch(const SparseBindBatch&) = delete;
    SparseBindBatch& operator=(const SparseBindBatch&) = delete;

    const std::uint64_t wait_value;
    const std::uint64_t signal_value;
    const vk::Semaphore semaphore;
    const vk::TimelineSemaphoreSubmitInfo timeline;
    const vk::BindSparseInfo info;
};

} // namespace VideoCore
