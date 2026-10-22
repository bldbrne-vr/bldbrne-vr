// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/fast_render.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include <source_location>
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/memory_policy.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache, PageManager& tracker);
    ~TextureCache();

    /// Allocation-failure recovery for VR scene storage (outside the cache lock).
    u64 ReclaimMemory(u64 bytes);

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        if (FastRender::On(FastRender::UpdateImageFast) &&
            slot_images[image_id].FastUpdateValid(gc_tick)) {
            return;
        }
        std::scoped_lock lock{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
        if (False(image.flags & ImageFlagBits::Dirty) &&
            True(image.flags & ImageFlagBits::Registered) &&
            image.track_addr == image.info.guest_address &&
            image.track_addr_end == image.info.guest_address + image.info.guest_size) {
            image.PublishFastUpdate(gc_tick);
        } else {
            image.InvalidateFastUpdate();
        }
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base);

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        // FAST_CPU_LRU_TICK (bbport texture_cache.cpp:1396-1402): an image GetImage or
        // TouchImageOnce touched in this GC period skips the touch, which would change nothing
        // (GetImageTouch). Only an image's first GetImage of a period calls out. Mostly the
        // recording thread; also a guest thread (VideoOutDriver::RegisterBuffers through
        // Presenter::RegisterVideoOutSurface), unlocked as the touch always was.
        if (lru_tick_skip && image.lru_touched_tick == gc_tick) {
            return image;
        }
        GetImageTouch(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// VR: the image `id` if it is still the registered image with this uid (ids are recycled),
    /// else nullptr. Does not touch the image's LRU state.
    [[nodiscard]] Image* TryGetImage(ImageId id, u64 uid) {
        if (!id || !slot_images.is_allocated(id)) {
            return nullptr;
        }
        auto& image = slot_images[id];
        return image.image_uid == uid && True(image.flags & ImageFlagBits::Registered) ? &image
                                                                                       : nullptr;
    }

    /// perf_bind_memo: what FindImage does to the image it found besides the lookup (its last
    /// access tick and its place in the LRU), for a remembered binding.
    void MarkFound(ImageId image_id);

    /// FAST bit 26: the image without GetImage's LRU touch (the bind memo's hit path touches
    /// each image itself, at most once per GC period).
    [[nodiscard]] Image& PeekImage(ImageId id) {
        return slot_images[id];
    }

    /// FAST bit 26 (bbport texture_cache.cpp:1396-1402): the LRU touch at most once per GC
    /// period per image, by an image-local tick; only that one takes the lock. Recording thread
    /// only (gc_tick moves only there). lru_touched_tick is also written by GetImage with
    /// FAST_CPU_LRU_TICK, on whichever thread calls it (see GetImageTouch), where GetImage
    /// touched the LRU unlocked before. True when it took the lock.
    bool TouchImageOnce(Image& image) {
        if (image.lru_touched_tick == gc_tick) {
            return false;
        }
        std::scoped_lock lock{mutex};
        if (False(image.flags & ImageFlagBits::Registered)) {
            return true; // its LRU entry is freed: never touch it
        }
        TouchImage(image);
        image.lru_touched_tick = gc_tick;
        return true;
    }

    /// FAST bit 26 (bbport texture_cache.h:103-105): MarkFound without a lock per call. The
    /// access tick is read only by FindImage's overlap resolution on this same thread (the memo
    /// path already stores it unlocked); the LRU part is TouchImageOnce. True when it locked.
    bool MarkFoundOnce(Image& image, u64 tick) {
        image.tick_accessed_last = tick;
        return TouchImageOnce(image);
    }

    /// FAST_BIND_MEMO_HIT_VERIFY: whether the image's LRU entry carries this GC period (what
    /// TouchImageOnce's image-local tick and UpdateImage's fast path both imply).
    [[nodiscard]] bool LruTouchedThisPeriod(const Image& image) {
        std::scoped_lock lock{mutex};
        return True(image.flags & ImageFlagBits::Registered) &&
               lru_cache.TickOf(image.lru_id) >= gc_tick;
    }

    /// The registry epoch: bumped by every RegisterImage / UnregisterImage. While it is
    /// unchanged, FindImage gives every request the same image (perf_bind_memo relies on it).
    [[nodiscard]] u64 RegistryEpoch() const noexcept {
        return registry_epoch.load(std::memory_order_relaxed);
    }

    /// FAST_CPU_PASS_MEMO: bumped whenever a metadata surface may gain a cleared slice (ClearMeta,
    /// TouchMeta with is_clear, a new surface_metas entry, which starts with its clear mask).
    /// While it is unchanged, a slice IsMetaCleared found clear of a clear stays so.
    [[nodiscard]] u64 MetaClearEpoch() const noexcept {
        return meta_clear_epoch.load(std::memory_order_relaxed);
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            meta_clear_epoch.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
                meta_clear_epoch.fetch_add(1, std::memory_order_relaxed);
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// The readbackLinearImages setting as the cache took it at construction: OnSubmit's ordered
    /// task then downloads images into guest memory (ProcessDownloadImages). Any thread.
    [[nodiscard]] bool ReadsBackLinearImages() const noexcept {
        return readback_linear_images;
    }

    // Called by A only after draining B. Exclude cached image footprints from
    // the constant ring, including later CPU download writes to those images.
    std::vector<std::pair<VAddr, u64>> ConstantImageExclusions() {
        std::vector<std::pair<VAddr, u64>> ranges;
        for (const auto& image : slot_images) {
            if (image.info.guest_address && image.info.guest_size)
                ranges.emplace_back(image.info.guest_address, image.info.guest_size);
            if (image.info.stencil_addr && image.info.stencil_size)
                ranges.emplace_back(image.info.stencil_addr, image.info.stencil_size);
        }
        return ranges;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector(u64 transition_request = 0);

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info);

    /// Caller holds mutex; only inactive guest-backed textures may be destroyed immediately.
    u64 ReclaimMemoryFor(VAddr addr, size_t size, u64 bytes,
                         u64 max_tick = std::numeric_limits<u64>::max(),
                         size_t image_limit = std::numeric_limits<size_t>::max());

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id, bool immediate = false);
    void EraseImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    /// GetImage's touch when its tick test did not skip it (FAST_CPU_LRU_TICK).
    void GetImageTouch(Image& image);

    void FreeImage(ImageId image_id,
                   std::source_location where = std::source_location::current()) {
        if constexpr (Common::Log::Enabled) {
            NoteTargetFreed(image_id, where.line());
        }
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }
    /// Freeze/blink hunt (logs-on builds): a render or depth target freed, and by which line
    /// (the chest items' scene depth changing identity, 2026-10-18).
    void NoteTargetFreed(ImageId image_id, u32 line);

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    TextureMemory::AgeCutoff reclaim_age;
    TextureMemory::TransitionCleanup transition_cleanup;
    /// FAST_CPU_LRU_TICK (on unless '0') without FAST_CPU_LRU_TICK_VERIFY=1, read once at
    /// start-up: GetImage skips the touch of an image touched in this GC period.
    static const bool lru_tick_skip;
    /// FAST_CPU_LRU_TICK's line: GetImageTouch's counts (recording owner, like the touch).
    struct LruTickStats {
        u64 touches{}, verified{}, mismatches{};
    };
    LruTickStats lru_tick_stats;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    bool readback_linear_images;
    PageTable page_table;
    /// FindImage's perfect matches (an image at exactly this address with this size, format and
    /// type), so a texture or target bound again skips the page-table walk. An entry is only valid
    /// for the registry_epoch it was made in: every RegisterImage / UnregisterImage bumps it, and
    /// the page table and the registered images' infos change nowhere else.
    struct FindImageMemo {
        VAddr guest_address{};
        u32 guest_size{};
        Extent3D size{};
        vk::Format pixel_format{};
        AmdGpu::ImageType type{};
        bool exact_fmt{};
        u64 epoch{};
        ImageId image_id{};
    };
    static constexpr size_t FindImageMemoSize = 1024;
    std::array<FindImageMemo, FindImageMemoSize> find_image_memo{};
    std::atomic<u64> registry_epoch{1}; // also bumped by FindDepthTarget's stencil, unlocked
    std::mutex mutex;
    std::mutex samplers_mutex;
    std::atomic<u64> sampler_generation{0}; // bumped whenever GC erases a sampler
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
    std::atomic<u64> meta_clear_epoch{1}; // MetaClearEpoch: with surface_metas' own threads
};

} // namespace VideoCore
