// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <xxhash.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/texture_cache/bump_height.h"

namespace VideoCore::BumpHeight {

Store::Store() : worker{[this](std::stop_token stop) { Work(stop); }} {}

Store::~Store() {
    worker.request_stop();
    queue_cv.notify_all();
}

bool Store::Eligible(const ImageInfo& info, Layout& layout) {
    const u32 size = info.size.width;
    if (!info.props.is_block || info.num_bits != 64 || info.num_samples != 1 ||
        info.tile_mode != AmdGpu::TileMode::Thin1DThin || info.props.is_volume ||
        info.resources.layers != 1 || info.size.height != size || size < MinSize ||
        size > MaxSize || (size & (size - 1)) != 0 ||
        (info.pixel_format != vk::Format::eBc1RgbaUnormBlock &&
         info.pixel_format != vk::Format::eBc1RgbUnormBlock)) {
        return false;
    }
    layout = {};
    layout.size = size;
    const u32 levels = std::min<u32>(info.resources.levels, MaxMips);
    for (u32 m = 0; m < levels && (size >> m) >= MinMipSize; ++m) {
        const auto& mip = info.mips_layout[m];
        layout.mips[m] = {mip.offset, (mip.pitch + 3) / 4, (mip.height + 3) / 4, mip.size};
        layout.levels = m + 1;
    }
    return layout.levels != 0;
}

void Store::Queue(Entry& entry, const u8* guest) {
    entry.bytes.resize(entry.guest_size);
    std::memcpy(entry.bytes.data(), guest, entry.guest_size);
    entry.state.store(State::Queued, std::memory_order_release);
    {
        std::scoped_lock lock{queue_mutex};
        queue.push_back(&entry);
    }
    queue_cv.notify_one();
}

s32 Store::Request(const ImageInfo& info, const Layout& layout, const u8* guest, u64 frame) {
    std::scoped_lock lock{mutex};
    if (budget_frame != frame) {
        budget_frame = frame;
        budget = MaxRequestsPerFrame;
    }
    if (const auto it = by_address.find(info.guest_address); it != by_address.end()) {
        Entry& entry = *entries[static_cast<size_t>(it->second)];
        const State state = entry.state.load(std::memory_order_acquire);
        // The same place, another texture or its copy gone: made again.
        if (state == State::Dropped || (state != State::Queued && state != State::Made &&
                                        entry.guest_size != info.guest_size)) {
            if (budget == 0) {
                return -1;
            }
            --budget;
            if (state == State::Ready || state == State::Uploaded) {
                total_bytes -= entry.bytes.size();
                ready_count.fetch_sub(1, std::memory_order_release);
            }
            entry.guest_size = info.guest_size;
            entry.layout = layout;
            Queue(entry, guest);
            pending.push_back(it->second);
        }
        return it->second;
    }
    if (budget == 0 || entries.size() >= MaxSlots) {
        return -1;
    }
    --budget;
    const s32 slot = static_cast<s32>(entries.size());
    entries.push_back(std::make_unique<Entry>());
    Entry& entry = *entries.back();
    entry.address = info.guest_address;
    entry.guest_size = info.guest_size;
    entry.layout = layout;
    by_address.emplace(info.guest_address, slot);
    slots[static_cast<size_t>(slot)].store(&entry, std::memory_order_release);
    Queue(entry, guest);
    pending.push_back(slot);
    return slot;
}

void Store::Work(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:BumpHeight");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    while (!stop.stop_requested()) {
        Entry* entry = nullptr;
        {
            std::unique_lock lock{queue_mutex};
            queue_cv.wait(lock, stop, [&] { return !queue.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            entry = queue.front();
            queue.pop_front();
        }
        entry->hash = XXH3_64bits(entry->bytes.data(), entry->bytes.size());
        const bool stonework =
            Derive(entry->bytes.data(), entry->bytes.size(), entry->layout, entry->stats);
        if (!stonework) {
            std::vector<u8>{}.swap(entry->bytes);
        }
        LOG_INFO(Render_Vulkan,
                 "VR bump height: {:#x} {}x{}: {} (relief {:.1f} texels, faces {:.2f}, joints "
                 "{:.2f}, {} stones holding {:.2f} of the faces)",
                 entry->address, entry->layout.size, entry->layout.size,
                 stonework ? "stonework, height made" : "not stonework", entry->stats.relief,
                 entry->stats.face, entry->stats.joint, entry->stats.stones,
                 entry->stats.stone_share);
        entry->state.store(stonework ? State::Made : State::Rejected, std::memory_order_release);
    }
}

// Room for `bytes` more: the least recently drawn copies go (their images keep the height until
// they are uploaded again; Scale gives 0 from now, so nothing reads it).
void Store::MakeRoom(u64 bytes) {
    while (total_bytes + bytes > MaxBytes) {
        Entry* oldest = nullptr;
        for (const auto& e : entries) {
            const State state = e->state.load(std::memory_order_acquire);
            if ((state == State::Ready || state == State::Uploaded) &&
                (oldest == nullptr || e->used_frame.load(std::memory_order_relaxed) <
                                          oldest->used_frame.load(std::memory_order_relaxed))) {
                oldest = e.get();
            }
        }
        if (oldest == nullptr) {
            return;
        }
        oldest->state.store(State::Dropped, std::memory_order_release);
        total_bytes -= oldest->bytes.size();
        ready_count.fetch_sub(1, std::memory_order_release);
        std::vector<u8>{}.swap(oldest->bytes);
    }
}

void Store::Poll(std::vector<std::pair<VAddr, u32>>& ready) {
    std::scoped_lock lock{mutex};
    std::erase_if(pending, [&](s32 slot) {
        Entry& entry = *entries[static_cast<size_t>(slot)];
        const State state = entry.state.load(std::memory_order_acquire);
        if (state == State::Queued) {
            return false;
        }
        if (state == State::Made) {
            MakeRoom(entry.bytes.size());
            total_bytes += entry.bytes.size();
            entry.state.store(State::Ready, std::memory_order_release);
            ready_count.fetch_add(1, std::memory_order_release);
            ready.emplace_back(entry.address, entry.guest_size);
        }
        return true;
    });
}

const u8* Store::Bytes(VAddr address, u32 guest_size, const u8* guest) {
    if (ready_count.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    std::scoped_lock lock{mutex};
    const auto it = by_address.find(address);
    if (it == by_address.end()) {
        return nullptr;
    }
    Entry& entry = *entries[static_cast<size_t>(it->second)];
    const State state = entry.state.load(std::memory_order_acquire);
    if (state != State::Ready && state != State::Uploaded) {
        return nullptr;
    }
    // Another texture at this place now (the game loaded over it): its own bytes, and this
    // height is gone.
    if (entry.guest_size != guest_size || XXH3_64bits(guest, guest_size) != entry.hash) {
        entry.state.store(State::Dropped, std::memory_order_release);
        total_bytes -= entry.bytes.size();
        ready_count.fetch_sub(1, std::memory_order_release);
        std::vector<u8>{}.swap(entry.bytes);
        return nullptr;
    }
    entry.state.store(State::Uploaded, std::memory_order_release);
    return entry.bytes.data();
}

} // namespace VideoCore::BumpHeight
