// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/sparse_pool_logic.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {

// FAST_BUFFER_SPARSE (FAST_BUFFER_ARENA=1 asks the device for sparse buffers): every cache buffer
// is a sparse buffer of its own, a view whose 64 KB blocks are bound to pieces of shared VRAM
// slabs, one piece per guest block. A buffer that merges others (BufferCache::CreateBuffer) binds
// the pieces they already have: no copy and no second allocation, where a merge used to allocate
// the whole new range and copy every old buffer into it (copy-on-grow: one pool became a 736 MB
// buffer re-created and copied at every few MB it grew, 2026-11-04). Each buffer keeps its own
// handle, so whatever tells buffers apart by handle (memos, barriers, the right-eye registry,
// the copy shader's grouping) works as before. Binds are queued and go out in one
// vkQueueBindSparse right before the next submission, which waits for them (SubmitBinds).
class SparsePool {
public:
    explicit SparsePool(const Vulkan::Instance& instance);
    ~SparsePool();

    SparsePool(const SparsePool&) = delete;
    SparsePool& operator=(const SparsePool&) = delete;

    /// The cache's buffers are sparse views (the device can, and FAST_BUFFER_ARENA=1).
    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// A new view for the block-aligned guest range [begin, begin + size): a sparse buffer whose
    /// blocks are bound (queued) to the pieces those guest blocks have, new ones where none.
    /// Null when VRAM has no slab left for it: the caller makes an ordinary buffer instead.
    /// `pieces` gets the view's pieces, one per block: ReleaseView gives exactly those back.
    vk::Buffer CreateView(VAddr begin, u64 size, vk::BufferUsageFlags usage,
                          std::vector<u32>& pieces);

    /// The view from `begin` with these pieces is gone (its buffer destroyed, the GPU done with
    /// it): its blocks lose it; a piece no view or twin binds goes back. Any order with twins.
    void ReleaseView(VAddr begin, const std::vector<u32>& pieces);

    /// Right-eye views (native stereo twins of sparse views). A twin of the view [begin,
    /// begin + size): a block with a right piece (per-eye data) binds it, any other block the
    /// LEFT piece: the right eye reads the left bytes there, which is what a twin made by copying
    /// the left buffer held. `private_bits` gets one bit per block, set where the twin binds a
    /// right piece. Null when VRAM has no slab (the caller makes an ordinary twin).
    vk::Buffer CreateTwin(VAddr begin, u64 size, vk::BufferUsageFlags usage,
                          std::vector<u64>& private_bits, std::vector<u32>& pieces);

    /// Right pieces for the blocks of [begin, begin + size) that have none, before a per-eye
    /// write there: their view-relative block numbers go to `fresh` (the caller copies the left
    /// bytes into them through a new twin made after this). All or nothing: false, and nothing
    /// changed, when VRAM has no slab.
    bool Privatize(VAddr view_begin, VAddr begin, u64 size, std::vector<u64>& fresh);

    /// Undo Privatize (the twin that was to bind its pieces could not be made).
    void Unprivatize(VAddr view_begin, const std::vector<u64>& fresh);

    /// A twin with these pieces is gone (destroyed, the GPU done with it): each loses a binding;
    /// a block's right piece no twin binds goes back, its per-eye bytes with it (as a released
    /// twin's). Any order with views.
    void ReleaseTwin(const std::vector<u32>& pieces);

    /// The bytes in right pieces (per-eye data).
    [[nodiscard]] u64 PrivateBytes() const {
        std::scoped_lock lock{mutex}; // twins are released on another thread
        return book.RightPieces() * SparseLogic::BlockSize;
    }

    /// Scheduler::SubmitLocked, under the queue lock: the queued binds in one vkQueueBindSparse
    /// on `queue`, chained to the previous bind batch. The value of Timeline() this and every
    /// later submission waits for (0: none); a completed value covers all preceding binds too.
    u64 SubmitBinds(vk::Queue queue);

    [[nodiscard]] vk::Semaphore Timeline() const noexcept {
        return *timeline;
    }

    /// Slabs with no piece in use for a while go back to the driver (one spare stays).
    void Collect(u64 now_ns);

    [[nodiscard]] u64 ResidentBytes() const {
        std::scoped_lock lock{mutex};
        return resident_pieces * SparseLogic::BlockSize;
    }
    /// The slabs allocated now (Collect frees empty ones; their slots stay in `slabs`).
    [[nodiscard]] u64 SlabBytes() const {
        std::scoped_lock lock{mutex};
        u64 live = 0;
        for (const Slab& slab : slabs) {
            live += slab.memory ? 1 : 0;
        }
        return live * SparseLogic::SlabSize;
    }

private:
    struct Slab {
        vk::DeviceMemory memory{};
        u32 used{};
        s64 empty_since_ns{}; // 0 while in use
        std::array<u64, SparseLogic::PiecesPerSlab / 64> free{}; // bit set: the piece is free
    };
    std::optional<u32> TakePiece();
    // The pool's lock, a wait for it timed (Common::Slow::PoolWait).
    std::unique_lock<std::mutex> Lock();
    void GivePiece(u32 piece);
    void QueueBinds(vk::Buffer buffer, const std::vector<u32>& pieces);

    const Vulkan::Instance& instance;
    bool enabled{};
    u32 memory_type{};
    vk::UniqueSemaphore timeline;
    u64 timeline_value{};
    u64 known_done{}; // the newest bind value known signalled
    mutable std::mutex mutex;
    std::vector<Slab> slabs; // the lowest free piece of the lowest slab first: adjacent binds merge
    u32 first_room{}; // no slab below it has a free piece (TakePiece starts here)
    SparseLogic::Book book; // the blocks' pieces and who binds them
    u64 resident_pieces{};
    std::vector<std::pair<vk::Buffer, std::vector<vk::SparseMemoryBind>>> pending;
};

} // namespace VideoCore
