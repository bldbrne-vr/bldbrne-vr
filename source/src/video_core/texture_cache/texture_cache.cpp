// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstring>
#include <xxhash.h>

#include "common/assert.h"
#include "common/slow_scope.h"
#include "common/crash_record.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/fast_render.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/download_log.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

namespace {
// FAST_CPU_LRU_TICK (on unless '0'; read once at start-up, a plain load per GetImage): GetImage
// touches an image's LRU entry at most once per GC period, as TouchImageOnce does (bbport
// TextureCache::TouchImage, texture_cache.cpp:1396-1402). It touched it on every call: an
// out-of-line call, a std::deque index (one 32-byte item per block, so _Getblock and a cold
// line per touch) and a compare, about 3% of Stage B (several calls per draw). A skipped touch
// is one Touch would have done nothing for: GetImageTouch.
const bool g_lru_tick = [] {
    const char* value = std::getenv("FAST_CPU_LRU_TICK");
    return !value || value[0] != '0';
}();
// FAST_CPU_LRU_TICK_VERIFY=1: no touch is skipped; each one the tick would have skipped is
// checked to change nothing (LOG_ERROR on a mismatch).
const bool g_lru_tick_verify = [] {
    const char* value = std::getenv("FAST_CPU_LRU_TICK_VERIFY");
    return value && value[0] == '1';
}();
} // namespace

const bool TextureCache::lru_tick_skip = g_lru_tick && !g_lru_tick_verify;

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy image_download = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.Commands();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), image_download);

    // FAST_CPU_GUARD: each write is logged (download_log.h), so the constants Stage A copied
    // before it are rebound by B from the guest bytes.
    if (sync) {
        scheduler.Finish();
        g_download_log.Begin(image.info.guest_address, download_size);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download, download_size);
        g_download_log.End();
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                g_download_log.Begin(device_addr, download_size);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr), download,
                                                          download_size);
                g_download_log.End();
            });
    }
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    image.InvalidateFastUpdate();
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.InvalidateFastUpdate();
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.InvalidateFastUpdate();
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id = InsertImage(new_info);
        RegisterImage(new_image_id);

        // Insertion may grow slot storage and move every image.
        auto& source_image = slot_images[cache_image_id];
        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = source_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;

        if (source_image.info.num_samples == 1 && new_info.num_samples == 1) {
            // Perform depth<->color copy using the intermediate copy buffer.
            // maintenance8 permits only specific color/depth format pairs, not arbitrary
            // equal-byte formats (e.g. RGBA8 -> D32S8 at reduced render resolutions).
            // Preserve bit reinterpretation through the buffer path for cross-aspect copies.
            if (instance.IsMaintenance8Supported() &&
                source_image.info.props.is_depth == new_info.props.is_depth) {
                new_image.CopyImage(source_image);
            } else {
                const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
                new_image.CopyImageWithBuffer(source_image, copy_buffer.Handle(), 0);
            }
        } else if (source_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            // Perform a rendering pass to transfer the channels of source as samples in dest.
            source_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height, new_info.num_samples,
                source_image.info.pixel_format, new_info.pixel_format, source_image.GetImage(),
                new_image.GetImage());
        } else {
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = InsertImage(info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    new_image.CopyImage(src_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    FastRender::AssertRecordingAccess();
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};
    ImageIds image_ids;
    ImageId image_id{};

    // A perfect match found before, while no image was registered or unregistered since, is
    // still the perfect match: skip the page-table walk.
    auto& memo = find_image_memo[((info.guest_address >> 8) ^ (info.guest_address >> 20) ^
                                  info.guest_size ^ static_cast<u64>(info.pixel_format) * 31) &
                                 (FindImageMemoSize - 1)];
    if (memo.epoch == registry_epoch.load() && memo.guest_address == info.guest_address &&
        memo.guest_size == info.guest_size && memo.size == info.size &&
        memo.pixel_format == info.pixel_format && memo.type == info.type &&
        memo.exact_fmt == exact_fmt) {
        image_id = memo.image_id;
    } else {
        ForEachImageInRegion(info.guest_address, info.guest_size,
                             [&](ImageId id, Image&) { image_ids.push_back(id); });

        // Check for a perfect match first
        for (const auto& cache_id : image_ids) {
            auto& cache_image = slot_images[cache_id];
            if (cache_image.info.guest_address != info.guest_address) {
                continue;
            }
            if (cache_image.info.guest_size != info.guest_size) {
                continue;
            }
            if (cache_image.info.size != info.size) {
                continue;
            }
            if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
                (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
                continue;
            }
            if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
                continue;
            }
            image_id = cache_id;
        }
        if (image_id) {
            memo = {.guest_address = info.guest_address,
                    .guest_size = info.guest_size,
                    .size = info.size,
                    .pixel_format = info.pixel_format,
                    .type = info.type,
                    .exact_fmt = exact_fmt,
                    .epoch = registry_epoch.load(),
                    .image_id = image_id};
        }
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = InsertImage(info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

void TextureCache::MarkFound(ImageId image_id) {
    std::scoped_lock lock{mutex};
    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer (a new entry starts cleared: MetaClearEpoch)
    if (desc.info.meta_info.cmask_addr) {
        if (surface_metas.emplace(desc.info.meta_info.cmask_addr,
                                  MetaDataInfo{.type = MetaType::CMask})
                .second) {
            meta_clear_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        if (surface_metas.emplace(desc.info.meta_info.fmask_addr,
                                  MetaDataInfo{.type = MetaType::FMask})
                .second) {
            meta_clear_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer (a new entry: MetaClearEpoch)
    if (desc.info.meta_info.htile_addr) {
        if (surface_metas
                .emplace(desc.info.meta_info.htile_addr,
                         MetaDataInfo{.type = MetaType::HTile,
                                      .clear_mask = image.info.meta_info.htile_clear_mask})
                .second) {
            meta_clear_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            std::scoped_lock lock{mutex};
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = InsertImage(info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, slot_images[image_id].image_uid);
    }

    // Creating a stencil may have moved the slot storage.
    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    Common::Slow::Scope slow{Common::Slow::RefreshImage, image.info.guest_size};
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    // FAST bit 39 copy-chain probe (counters only): bytes an image-sourced copy just wrote are
    // uploaded into an image again.
    if (FastRender::On(TileManager::TileFromImage)) [[unlikely]] {
        TileManager::NoteImageRefresh(image);
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    if (auto barrier = in_buffer->GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                             vk::PipelineStageFlagBits2::eTransfer)) {
        scheduler.Commands().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    // FAST bit 61 (TileManager::UploadDetile): a tiled colour image is detiled straight into its
    // layers, with Upload's final state; nothing was recorded when it returns false.
    if (FastRender::On(TileManager::UploadDetile) &&
        tile_manager.DetileIntoImage(
            image, std::span<const vk::BufferImageCopy>{image_copies.data(), image_copies.size()},
            in_buffer->Handle(), in_offset)) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    const auto [buffer, offset] =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    image.Upload(image_copies, buffer, offset);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    // FAST bit 23: a per-thread direct-mapped memo skips the hash, mutex, map lookup and LRU
    // touch per sampler per draw. Garbage collection bumps the generation (retired samplers
    // are destroyed through deferred operations); hits refresh the LRU every 256th use.
    static constexpr auto SamplerMemo = static_cast<FastRender::Bit>(23);
    static_assert(sizeof(AmdGpu::Sampler) == 16);
    struct MemoEntry {
        AmdGpu::Sampler key;
        u64 generation;
        vk::Sampler handle;
        u32 uses;
        bool valid;
    };
    thread_local std::array<MemoEntry, 64> memo{};
    MemoEntry* entry = nullptr;
    if (FastRender::On(SamplerMemo)) {
        u64 words[2];
        std::memcpy(words, &sampler, sizeof(words));
        entry = &memo[((words[0] ^ (words[1] * 0x9E3779B97F4A7C15ull)) * 0xC2B2AE3D27D4EB4Full) >> 58];
        if (entry->valid &&
            entry->generation == sampler_generation.load(std::memory_order_acquire) &&
            std::memcmp(&entry->key, &sampler, sizeof(sampler)) == 0 && (++entry->uses & 255) != 0)
            return entry->handle;
    }
    const u64 hash = XXH3_64bits(&sampler, sizeof(sampler));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }
    if (entry)
        *entry = {sampler, sampler_generation.load(std::memory_order_relaxed), it->second.Handle(), 1,
                  true};
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_epoch; // FindImage's memo
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    image.InvalidateFastUpdate();
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_epoch; // FindImage's memo
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.InvalidateFastUpdate();
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.InvalidateFastUpdate();
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.InvalidateFastUpdate();
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    if constexpr (Common::Log::Enabled) {
        // Experiments (logs-on builds only): VR_TEXTURE_GC_FILE names a file read once a second;
        // "0" in it turns the collector off live, anything else (or no file) leaves it on.
        static const char* const file = std::getenv("VR_TEXTURE_GC_FILE");
        static u64 checked_second = 0;
        static bool enabled = true;
        if (file != nullptr) {
            const u64 now_second = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (now_second != checked_second) {
                checked_second = now_second;
                bool on = true;
                if (FILE* f = std::fopen(file, "r")) {
                    on = std::fgetc(f) != '0';
                    std::fclose(f);
                }
                if (on != enabled) {
                    enabled = on;
                    LOG_WARNING(Render_Vulkan, "Texture GC: {} (VR_TEXTURE_GC_FILE)",
                                on ? "ON" : "OFF");
                }
            }
            if (!enabled) {
                return;
            }
        }
    }
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    u32 evictions = 0, writebacks = 0;
    u64 writeback_us = 0;
    // Each synchronous write-back below is a full GPU wait under this lock: at most this many per
    // pass bounds the hitch on a GPU short of memory (a pass may evict 20-40 images). Later ones
    // stay first in line, untouched, for the next pass a frame later (2026-10-18).
    constexpr u32 MaxSyncWritebacksPerPass = 4;
    u32 sync_writebacks = 0;
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const u64 second = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
    u64 below_tick = 0;
    // emergency: still over the critical mark after evicting what was unused for a second.
    const auto configure = [&](bool allow_aggressive, bool emergency = false) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!emergency) {
            below_tick = std::min(
                below_tick,
                reclaim_age.Before(second, TextureMemory::IdleSeconds(pressured, aggresive)));
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto can_evict = [&](ImageId image_id) {
        const auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        // As in bbport: skipped images neither spend the budget nor stay first in line.
        // Keep GPU-written tiled images: this branch cannot download them correctly.
        if ((image.info.IsTiled() && download) || (download && !pressured)) {
            lru_cache.Touch(image.lru_id, gc_tick);
            return false;
        }
        if (download && sync_writebacks >= MaxSyncWritebacksPerPass) {
            return false;
        }
        return true;
    };
    const auto clean_up = [&](ImageId image_id) {
        const bool download = slot_images[image_id].SafeToDownload();
        if (download) {
            ++sync_writebacks;
            // bbport: finish the write-back while the image still protects its guest pages.
            // A deferred write-back after FreeImage could overwrite newly reused guest memory.
            const auto started = Common::Log::Enabled ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
            DownloadImageMemory(image_id, true);
            if constexpr (Common::Log::Enabled) {
                ++writebacks;
                writeback_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - started).count();
            }
        }
        FreeImage(image_id);
        ++evictions;
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    TextureMemory::Collect(lru_cache, below_tick, num_deletions, can_evict, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        TextureMemory::Collect(lru_cache, below_tick, num_deletions, can_evict, clean_up);
    }
    if (total_used_memory >= critical_gc_memory) {
        // Still over it: what was used a moment ago goes too (out of memory otherwise).
        configure(true, true);
        TextureMemory::Collect(lru_cache, below_tick, num_deletions, can_evict, clean_up);
    }
    if constexpr (Common::Log::Enabled) {
        if (evictions != 0) {
            struct Stats { u64 evictions{}, writebacks{}, writeback_us{}, next_report{}; };
            static thread_local Stats stats;
            stats.evictions += evictions;
            stats.writebacks += writebacks;
            stats.writeback_us += writeback_us;
            const u64 second = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (second >= stats.next_report) {
                LOG_WARNING(Render_Vulkan, "Texture GC: evictions={} writebacks={} writeback_us={} "
                            "used_estimate_mib={}", stats.evictions, stats.writebacks,
                            stats.writeback_us, total_used_memory >> 20);
                stats = {.next_report = second + 5};
            }
        }
    }

}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        auto retired = std::move(samplers.at(hash));
        samplers.erase(hash);
        sampler_generation.fetch_add(1, std::memory_order_release);
        scheduler.DeferOperation([retired = std::move(retired)]() {});
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector(u64 transition_request) {
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    reclaim_age.Record(static_cast<u64>(second), gc_tick);
    SCOPE_EXIT {
        ++gc_tick;
    };

    if (const auto cutoff = transition_cleanup.Step(second, transition_request, gc_tick, [&] {
            // Same threshold and accounting as normal GC. Query only when a pass is due.
            const u64 usage = instance.CanReportMemoryUsage() ? instance.GetDeviceMemoryUsage()
                                                            : total_used_memory;
            return usage >= pressure_gc_memory;
        })) {
        const auto started = std::chrono::steady_clock::now();
        u64 freed;
        {
            std::scoped_lock lock{mutex};
            freed = ReclaimMemoryFor(0, 0, u64{256} << 20, *cutoff, 128);
        }
        LOG_INFO(Render_Vulkan, "Texture transition cleanup: request={} released={} MiB ms={:.2f}",
                 transition_request, freed >> 20,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                     .count());
    }
    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::GetImageTouch(Image& image) {
    if (!g_lru_tick) {
        TouchImage(image); // FAST_CPU_LRU_TICK=0: every GetImage touches, as before
        return;
    }
    auto& s = lru_tick_stats;
    // gc_tick is read once: GetImage also runs on a guest thread (Presenter::
    // RegisterVideoOutSurface) while the recording owner may advance it (RunGarbageCollector,
    // OnSubmit's ordered task). The tick stored below must be the one the entry was touched at,
    // or a touch at G followed by a store of G + 1 would skip the image's touches for all of
    // period G + 1.
    const u64 tick = gc_tick;
    const auto report = [&] {
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_LRU_TICK: GetImage LRU touches={} verified={} mismatches={} "
                 "(gc tick {})",
                 s.touches, s.verified, s.mismatches, tick);
    };
    if (image.lru_touched_tick == tick) {
        // FAST_CPU_LRU_TICK_VERIFY (GetImage skips nothing then): a touch the tick skips. Touch
        // changes an entry only when its tick is below gc_tick, so this one must find it at
        // gc_tick or above. It is done all the same.
        const u64 entry_tick = lru_cache.TickOf(image.lru_id);
        lru_cache.Touch(image.lru_id, tick);
        if constexpr (Common::Log::Enabled) {
            ++s.verified;
            if (entry_tick < tick && ++s.mismatches <= 64) {
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_LRU_TICK verify: image {:#x} (uid {}, registered {}) was "
                          "touched in GC period {} but its LRU entry {} is at {}; the touch is "
                          "done, mismatch #{}",
                          image.info.guest_address, image.image_uid,
                          True(image.flags & ImageFlagBits::Registered), tick, image.lru_id,
                          entry_tick, s.mismatches);
            }
            if ((s.verified & 0x3FFFFF) == 0) {
                report();
            }
        }
        return;
    }
    // Its entry's tick is now at least gc_tick (Touch raised it, or Insert set it when this
    // registration began), and entry ticks only rise: until RunGarbageCollector's ++gc_tick,
    // touching it again changes nothing. That holds for every entry this image may own in the
    // period, as a registration begun meanwhile inserts at gc_tick, and for its freed entry
    // (Free keeps the tick, a reuse inserts at gc_tick), so a skip is exact in every case the
    // touch is. Same lock rules as the touch (none: TouchImage's); with the one read above, a
    // guest thread's touch racing the increment stores the period it touched in, so the
    // recording owner's next GetImage touches again, as it did before the switch.
    lru_cache.Touch(image.lru_id, tick); // TouchImage at `tick`
    image.lru_touched_tick = tick;
    if constexpr (Common::Log::Enabled) {
        if ((++s.touches & 0xFFFF) == 0) {
            report();
        }
    }
}

void TextureCache::NoteTargetFreed(ImageId image_id, u32 line) {
    const Image& image = slot_images[image_id];
    if (!image.usage.render_target && !image.usage.depth_target) {
        return;
    }
    static std::atomic<u64> count{0};
    const u64 n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 64 || (n & 255) == 0) {
        LOG_INFO(Render_Vulkan,
                 "Texture target freed #{}: {}x{} {:#x} {} uid {} flags {:#x} unused {} ticks "
                 "(lru {}), freed at texture_cache line {}",
                 n + 1, image.info.size.width, image.info.size.height, image.info.guest_address,
                 image.usage.depth_target ? "depth" : "colour", image.image_uid,
                 static_cast<u32>(image.flags), scheduler.CurrentTick() - image.tick_accessed_last,
                 gc_tick - lru_cache.TickOf(image.lru_id), line);
    }
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
    const auto image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    if (!slot_images[image_id].allocation_failed) {
        return image_id;
    }
    u64 freed = 0;
    const bool recovered = TextureMemory::Recover(
        info.guest_size,
        [&](u64 bytes) { freed += ReclaimMemoryFor(info.guest_address, info.guest_size, bytes); },
        [&] { return slot_images[image_id].RetryAllocation(); });
    LOG_WARNING(Render_Vulkan, "Texture allocation recovery: {} MiB released, recovered={}",
                freed >> 20, recovered);
    if (!recovered) {
        Common::CrashRecord::Note("out of memory: texture allocation failed after cache cleanup");
    }
    ASSERT_MSG(recovered, "Failed allocating image after texture-cache recovery");
    return image_id;
}

u64 TextureCache::ReclaimMemory(u64 bytes) {
    std::scoped_lock lock{mutex};
    return ReclaimMemoryFor(0, 0, bytes);
}

u64 TextureCache::ReclaimMemoryFor(VAddr addr, size_t size, u64 bytes, u64 max_tick,
                                 size_t image_limit) {
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const u64 cutoff = std::min(max_tick, reclaim_age.Before(static_cast<u64>(second)));
    const u64 open_tick = scheduler.CurrentTick();
    boost::container::small_vector<ImageId, 64> victims;
    u64 freed = 0;
    lru_cache.ForEachItemBelow(cutoff, [&](ImageId id) {
        if (freed >= bytes || victims.size() >= image_limit) {
            return true;
        }
        const auto& image = slot_images[id];
        const TextureMemory::Candidate candidate{
            .lru_tick = lru_cache.TickOf(image.lru_id),
            .submission_tick = image.tick_accessed_last,
            .gpu_valid_data = image.SafeToDownload(),
            .retained = bool(image.scene_proxy) || image.usage.vo_surface ||
                        image.binding.is_bound || image.binding.is_target ||
                        True(image.flags & ImageFlagBits::Picked),
            .overlaps = size != 0 && image.Overlaps(addr, size),
        };
        if (TextureMemory::CanReclaim(candidate, cutoff, open_tick)) {
            victims.push_back(id);
            // Use actual VMA sizes: stereo layers and extra sample backings also consume memory.
            for (const auto& backing : image.backing_images) {
                freed += backing.image.SizeBytes();
            }
        }
        return false;
    });
    if (victims.empty()) {
        return 0;
    }
    // Never submit the partially prepared draw: only wait for earlier, already submitted ticks.
    // All unsubmitted uses are protected by the LRU/current-tick and binding checks above.
    scheduler.WaitSubmitted();
    for (const auto id : victims) {
        UntrackImage(id);
        UnregisterImage(id);
        DeleteImage(id, true);
    }
    return freed;
}

void TextureCache::DeleteImage(ImageId image_id, bool immediate) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    if (immediate) {
        EraseImage(image_id);
    } else {
        scheduler.DeferOperation([this, image_id] { EraseImage(image_id); });
    }
}

void TextureCache::EraseImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    for (auto& backing : image.backing_images) {
        for (const ImageViewId image_view_id : backing.image_view_ids) {
            slot_image_views.erase(image_view_id);
        }
    }
    slot_images.erase(image_id);
}

} // namespace VideoCore
