// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/texture_cache/bump_height_derive.h"
#include "video_core/texture_cache/image_info.h"

// The heights made for bump maps that have none (bump_height_derive.h: Central Yharnam's cobbles,
// most brick walls), from first sight to the image:
//  - Request (the rasterizer, when a G-buffer draw binds a bump map whose blue is flat): the
//    texture's guest bytes are copied (one memcpy, MaxRequestsPerFrame textures a frame at most)
//    and queued; a slot comes back;
//  - a single low-priority thread makes the height (about 10 ms a texture; nine in ten are not
//    stonework and stop there) into that copy's blue;
//  - Poll (once a frame) hands back the textures that became ready, for the texture cache to
//    upload again;
//  - Bytes (TextureCache::RefreshImage): the upload then takes the copy instead of the guest's
//    bytes, when the guest's are still the ones it was made from (their hash);
//  - Made (the rasterizer, per draw): whether the image holds the height yet. A draw before
//    that, and of everything that is not stonework, is drawn as the game draws it.
// The game's memory is never written. The copies are bounded (MaxBytes: the least recently drawn
// go first; drawn again, they are made again).
namespace VideoCore::BumpHeight {

class Store {
public:
    Store();
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // A texture a height can be made for (BC1, micro-tiled, one layer, square, a power of two of
    // MinSize to MaxSize) and where its levels lie.
    static bool Eligible(const ImageInfo& info, Layout& layout);

    // The texture's slot, made (and its height queued) at first sight; -1: none this frame (the
    // frame's copies are spent, or the slots are): ask again later.
    s32 Request(const ImageInfo& info, const Layout& layout, const u8* guest, u64 frame);

    // Whether the image holds the slot's height (a draw of it then takes the depth).
    bool Made(s32 slot, u64 frame) {
        Entry* const e = slots[static_cast<size_t>(slot)].load(std::memory_order_acquire);
        if (e == nullptr || e->state.load(std::memory_order_acquire) != State::Uploaded) {
            return false;
        }
        e->used_frame.store(frame, std::memory_order_relaxed);
        return true;
    }

    // Once a frame: the address and size of each texture whose height became ready.
    void Poll(std::vector<std::pair<VAddr, u32>>& ready);

    // The bytes to upload for the image at `address` in place of `guest`'s, nullptr: the guest's.
    const u8* Bytes(VAddr address, u32 guest_size, const u8* guest);

    static constexpr size_t MaxSlots = 4096;
    static constexpr u32 MaxRequestsPerFrame = 2;
    static constexpr u64 MaxBytes = 64ull << 20;

private:
    enum class State : u8 {
        Queued,   // its copy is the worker's
        Made,     // stonework: its copy holds the height, not yet counted
        Ready,    // waiting for its upload
        Uploaded, // the image holds it
        Rejected, // not stonework: nothing kept
        Dropped,  // the guest's bytes changed, or its copy made room: asked again, made again
    };
    struct Entry {
        VAddr address{};
        u32 guest_size{};
        Layout layout{};
        std::vector<u8> bytes;
        u64 hash{}; // of the guest's bytes as copied
        Stats stats{};
        std::atomic<State> state{State::Queued};
        std::atomic<u64> used_frame{};
    };

    void Work(std::stop_token stop);
    void Queue(Entry& entry, const u8* guest);
    void MakeRoom(u64 bytes);

    std::array<std::atomic<Entry*>, MaxSlots> slots{};
    std::mutex mutex; // entries, by_address, pending, total_bytes, the frame's budget
    std::vector<std::unique_ptr<Entry>> entries;
    std::unordered_map<VAddr, s32> by_address;
    std::vector<s32> pending;
    u64 total_bytes{};
    u64 budget_frame{};
    u32 budget{};
    std::atomic<u32> ready_count{}; // Ready or Uploaded entries (Bytes' quick way out)

    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::deque<Entry*> queue;
    std::jthread worker;
};

} // namespace VideoCore::BumpHeight
