// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <mutex>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/fast_render.h"

namespace VideoCore {

struct ImageInfo;
struct Image;
class StreamBuffer;

/// FAST bit 39 (TileManager::TileFromImage) counters, the 'FAST tile_from_image:' line. Relaxed
/// atomics: re-tiles run on the recording thread, image refreshes wherever an image is updated.
struct TileFromImageStats {
    std::atomic<u64> events{0};
    std::atomic<u64> from_image{0};       // re-tiles that read the image (no linear download)
    std::atomic<u64> from_image_bytes{0}; // their guest bytes, both eyes
    std::atomic<u64> fallback_kind{0};    // depth/stencil, block, MSAA, mips, layers, volume
    std::atomic<u64> fallback_format{0};  // host texel size is not the guest's, or 96 bits
    std::atomic<u64> fallback_copy{0};    // not one whole-mip copy laid out with the guest pitch
    std::atomic<u64> fallback_check{0};   // VR_PERF_CHECK compares padding bytes: download kept
    std::atomic<u64> fallback_failed{0};  // shader compile / pipeline / view creation failed
    std::atomic<u64> verified{0};         // FAST_TILE_FROM_IMAGE_VERIFY comparisons done
    std::atomic<u64> mismatches{0};       // ... that differed
    // The copy-chain probe (PLAN_V3 D2): image-sourced buffer writes of the 3d5ebf4e direct path,
    // how many of their destinations were uploaded into an image again (RefreshImage), and of
    // those how many with the source's exact tiled layout (an image-to-image copy could replace
    // the re-tile + detile + upload there).
    std::atomic<u64> chain_writes{0};
    std::atomic<u64> chain_reuploads{0};
    std::atomic<u64> chain_same_layout{0};
    // FAST bit 60 (TileManager::TileDepthFromImage), the 'FAST tile_depth_from_image:' line: depth
    // images re-tiled with the bit on (eligible), read straight from the depth aspect (taken), and
    // the ones that kept the download, by reason.
    std::atomic<u64> depth_eligible{0};
    std::atomic<u64> depth_from_image{0};
    std::atomic<u64> depth_from_image_bytes{0}; // both eyes
    std::atomic<u64> depth_fallback_kind{0};    // MSAA, mips, layers, volume, no depth aspect
    std::atomic<u64> depth_fallback_format{0};  // not 32-bit float depth (D32Sfloat[S8Uint])
    std::atomic<u64> depth_fallback_copy{0};    // not one whole-mip depth-aspect copy
    std::atomic<u64> depth_fallback_check{0};   // VR_PERF_CHECK: download kept
    std::atomic<u64> depth_fallback_failed{0};  // shader compile / pipeline / view creation failed
    std::atomic<u64> depth_verified{0};         // FAST_TILE_DEPTH_FROM_IMAGE_VERIFY comparisons
    std::atomic<u64> depth_mismatches{0};       // ... that differed
};
inline TileFromImageStats g_tile_from_image_stats;

/// FAST bit 39 copy-chain probe: the last image-sourced buffer writes (destination address and the
/// source's tiled layout key). Diagnostic only; a racing reader at worst misses or double-counts.
class ImageSourcedWrites {
public:
    static constexpr u32 Size = 8;
    void Note(VAddr dst, u64 layout) noexcept {
        if (dst == 0) {
            return;
        }
        const u32 slot = next.fetch_add(1, std::memory_order_relaxed) % Size;
        layouts[slot].store(layout, std::memory_order_relaxed);
        addresses[slot].store(dst, std::memory_order_release);
    }
    /// The entry for `dst`, removed (one count per write).
    bool Take(VAddr dst, u64& layout) noexcept {
        if (dst == 0) {
            return false;
        }
        for (u32 i = 0; i < Size; ++i) {
            u64 expected = dst;
            if (addresses[i].load(std::memory_order_acquire) == dst &&
                addresses[i].compare_exchange_strong(expected, u64{0},
                                                     std::memory_order_acq_rel)) {
                layout = layouts[i].load(std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::atomic<u64>, Size> addresses{};
    std::array<std::atomic<u64>, Size> layouts{};
    std::atomic<u32> next{0};
};
inline ImageSourcedWrites g_image_sourced_writes;

/// FAST bit 61 (TileManager::UploadDetile) counters, the 'FAST upload_detile:' line. Relaxed
/// atomics; uploads are recorded on the recording thread, the verify results land in deferred
/// operations.
struct UploadDetileStats {
    std::atomic<u64> events{0};
    std::atomic<u64> taken{0};            // detiled straight into the image (no scratch + copy)
    std::atomic<u64> taken_bytes{0};      // their guest bytes, both eyes
    std::atomic<u64> fallback_linear{0};  // not tiled: the copy reads guest memory directly
    std::atomic<u64> fallback_kind{0};    // depth/stencil, block, MSAA, mips, layers, volume, usage
    std::atomic<u64> fallback_format{0};  // texel size, no storage support for the uint view
    std::atomic<u64> fallback_copy{0};    // not one whole-mip copy with the guest pitch, coverage
    std::atomic<u64> fallback_failed{0};  // shader compile / pipeline / view creation failed
    std::atomic<u64> verified{0};         // FAST_UPLOAD_DETILE_VERIFY comparisons done (per eye)
    std::atomic<u64> mismatches{0};       // ... that differed
};
inline UploadDetileStats g_upload_detile_stats;

class TileManager {
    static constexpr size_t NUM_BPPS = 5;

public:
    using ScratchBuffer = std::pair<vk::Buffer, VmaAllocation>;
    using Result = std::pair<vk::Buffer, u32>;

    /// FAST bit 39: a tiled colour image re-tiled into a guest buffer (TileImage: the 3d5ebf4e
    /// direct image-to-buffer copy, texel-buffer resyncs) is read by the tiler itself through a
    /// same-size R*_UINT view, instead of being downloaded into a linear scratch buffer first
    /// (bbport docs/parallel_gpu.md:542-548 names this
    /// image-sourced copy chain as its next GPU item). Off: exactly the download + tiler of before.
    static constexpr auto TileFromImage = static_cast<FastRender::Bit>(39);
    /// FAST bit 60 (needs bit 39): the same for a 32-bit float depth image (gpu_plan #5, R1; bbport
    /// docs/parallel_gpu.md:542-547): the tiler samples the depth aspect of the D32Sfloat[S8Uint]
    /// image and stores floatBitsToUint of it, the bytes copyImageToBuffer writes for the depth
    /// aspect, instead of the 'image download' into a linear scratch buffer. Off: depth images
    /// keep the download + tiler of before (bit 39's 'kind' fallback).
    static constexpr auto TileDepthFromImage = static_cast<FastRender::Bit>(60);

    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   vk::Buffer out_buffer, u32 out_offset, u32 copy_size);

    Result DetileImage(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info);

    /// FAST bit 61 (gpu_plan.md #6, research_gpu_copies.md R3; the mirror of bit 39): a tiled
    /// colour image refreshed from guest memory is detiled by the detiler itself into the image
    /// through a same-size R*_UINT storage view, instead of into a linear scratch buffer followed
    /// by copyBufferToImage per eye. Off: exactly DetileImage + Image::Upload of before.
    static constexpr auto UploadDetile = static_cast<FastRender::Bit>(61);

    /// Bit 61, from TextureCache::RefreshImage after its in_buffer barrier: true when the image
    /// was written (both eyes) and left in Image::Upload's final state; the caller then clears
    /// the Dirty flags. False: nothing was recorded; the caller takes DetileImage + Upload.
    bool DetileIntoImage(Image& image, std::span<const vk::BufferImageCopy> copies,
                         vk::Buffer in_buffer, u32 in_offset);

    /// Scratch buffers used and created since the last call (VR_STREAM_STATS).
    std::pair<u32, u32> TakeScratchStats() {
        const std::pair<u32, u32> r{scratch_uses, scratch_creates};
        scratch_uses = scratch_creates = 0;
        return r;
    }

    /// Bit 39 copy-chain probe: what decides the guest bytes of a tiled image (FNV-1a mix).
    static u64 TiledLayoutKey(const ImageInfo& info) noexcept;
    /// Bit 39 copy-chain probe, from TextureCache::RefreshImage before an upload from memory.
    static void NoteImageRefresh(const Image& image);

private:
    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler);
    /// The download + tiler path (the only one before bit 39). `zero_linear`: the linear scratch
    /// is cleared first, so padding positions are written as zeros (the bit 39 verify mode).
    void TileImageViaBuffer(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u32 out_offset, u32 copy_size,
                            bool zero_linear);
    /// Bit 39: false when the image or the copy is not eligible or a Vulkan object could not be
    /// made; nothing was recorded then and the caller takes the download path.
    bool TileImageFromImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u32 out_offset, u32 copy_size);
    /// `depth` (bit 60): read the depth aspect of a 32-bit float depth image.
    bool RetileFromImage(Image& in_image, u32 width, u32 height, vk::Buffer out_buffer,
                         u32 out_offset, bool depth);
    void VerifyFromImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                         u32 copy_size, u32 width, u32 height, bool depth);
    vk::Pipeline GetFromImagePipeline(const ImageInfo& info, bool depth);
    bool CreateFromImageLayout();
    /// Bit 61: records the detile of `in_buffer` (and its right copy) into the image's layers.
    bool RecordDetileIntoImage(Image& image, u32 width, u32 height, vk::Buffer in_buffer,
                               u32 in_offset);
    void VerifyDetileIntoImage(Image& image, const vk::BufferImageCopy& copy,
                               vk::Buffer in_buffer, u32 in_offset);
    vk::Pipeline GetToImagePipeline(const ImageInfo& info);
    bool CreateToImageLayout();
    bool ToImageFormatSupported(vk::Format view_format, u32 bits);
    /// A scratch buffer of at least `size` bytes: a pooled one of the same size class (a power of
    /// two, at least 64 KB) when the GPU is done with it, else a new one.
    ScratchBuffer GetScratchBuffer(u32 size);
    /// Gives a scratch buffer back once the GPU is done with it (a deferred operation).
    void ReleaseScratchBuffer(ScratchBuffer buffer, u32 size);
    static u32 ScratchSizeClass(u32 size);

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    StreamBuffer& stream_buffer;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> detilers{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> tilers{};
    // Bit 39, made on first use (on the recording thread, as the pipelines above).
    vk::UniqueDescriptorSetLayout from_image_desc_layout;
    vk::UniquePipelineLayout from_image_pl_layout;
    bool from_image_layout_failed{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> from_image_tilers{};
    std::array<bool, AmdGpu::NUM_TILE_MODES * NUM_BPPS> from_image_failed{};
    // Bit 60: the 32-bit depth variants (one per tile mode), made on first use the same way.
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES> from_depth_tilers{};
    std::array<bool, AmdGpu::NUM_TILE_MODES> from_depth_failed{};
    // Bit 61, made on first use (on the recording thread, as the pipelines above).
    vk::UniqueDescriptorSetLayout to_image_desc_layout;
    vk::UniquePipelineLayout to_image_pl_layout;
    bool to_image_layout_failed{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> to_image_detilers{};
    std::array<bool, AmdGpu::NUM_TILE_MODES * NUM_BPPS> to_image_failed{};
    // Storage support of the five uint view formats (R8, R16, R32, R32G32, R32G32B32A32 UINT):
    // 0 unknown, 1 yes, 2 no.
    std::array<u8, 5> to_image_format_support{};

    struct PooledScratch {
        u32 size_class;
        ScratchBuffer buffer;
    };
    static constexpr u64 MaxPooledScratchBytes = 512_MB;
    static constexpr size_t MaxPooledScratchBuffers = 32;
    std::mutex scratch_mutex;
    std::vector<PooledScratch> free_scratch;
    u64 pooled_scratch_bytes{};
    u32 scratch_uses{};
    u32 scratch_creates{};
};

} // namespace VideoCore
