// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/upload_staging_state.h"

namespace VideoCore {

// Owned and called by the command producer, like the buffer cache. Returned buffers
// remain alive through their submission; callers must record their use in that tick.
class UploadStagingPool {
public:
    struct Allocation {
        Buffer* buffer;
        u8* data;
        u64 offset;
        u64 size;
        UploadStaging::BlockState* state{};

        // Async writers require coherent memory. Flush synchronous writes before
        // recording GPU use, then retire only after the consuming commands.
        void Flush() const;
        void Retire() const;
    };

    UploadStagingPool(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    Allocation Request(u64 size, u64 alignment = 4);
    void TickFrame();

private:
    struct Block {
        std::unique_ptr<Buffer> buffer;
        UploadStaging::BlockState state;
    };

    Block& AddBlock(u64 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // Stable state pointers even if a source read recursively allocates staging.
    std::vector<std::unique_ptr<Block>> blocks;
    size_t current{};
    u64 frame{};
    u64 allocated_bytes{};
    u64 peak_bytes{};
    u64 allocations{};
    u64 requests{};
    u64 upload_bytes{};
    const bool stats;
};

} // namespace VideoCore
