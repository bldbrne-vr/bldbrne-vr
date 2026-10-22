#include "video_core/native_stereo/profile_scopes.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_comp.h"

#include <cstdlib>
#include <cstring>
#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() {
    for (const auto& pooled : free_scratch) {
        NativeStereo::ForgetBuffer(pooled.buffer.first);
        vmaDestroyBuffer(instance.GetAllocator(), pooled.buffer.first, pooled.buffer.second);
    }
}

u32 TileManager::ScratchSizeClass(u32 size) {
    return std::bit_ceil(std::max<u32>(size, static_cast<u32>(64_KB)));
}

void TileManager::ReleaseScratchBuffer(ScratchBuffer buffer, u32 size) {
    const u32 size_class = ScratchSizeClass(size);
    std::vector<ScratchBuffer> evicted;
    {
        std::scoped_lock lk{scratch_mutex};
        // Newest at the back; over the budget, the least recently released go first.
        free_scratch.push_back({size_class, buffer});
        pooled_scratch_bytes += size_class;
        while (pooled_scratch_bytes > MaxPooledScratchBytes ||
               free_scratch.size() > MaxPooledScratchBuffers) {
            evicted.push_back(free_scratch.front().buffer);
            pooled_scratch_bytes -= free_scratch.front().size_class;
            free_scratch.erase(free_scratch.begin());
        }
    }
    for (const auto& b : evicted) {
        NativeStereo::ForgetBuffer(b.first);
        vmaDestroyBuffer(instance.GetAllocator(), b.first, b.second);
    }
}

TileManager::ScratchBuffer TileManager::GetScratchBuffer(u32 size) {
    const u32 size_class = ScratchSizeClass(size);
    ++scratch_uses;
    {
        std::scoped_lock lk{scratch_mutex};
        // The most recently released one of this size class.
        const auto rit = std::ranges::find_if(
            free_scratch.rbegin(), free_scratch.rend(),
            [&](const PooledScratch& p) { return p.size_class == size_class; });
        const auto it = rit == free_scratch.rend() ? free_scratch.end() : std::prev(rit.base());
        if (it != free_scratch.end()) {
            const ScratchBuffer buffer = it->buffer;
            pooled_scratch_bytes -= size_class;
            free_scratch.erase(it);
            return buffer;
        }
    }
    ++scratch_creates;
    size = size_class;
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    ASSERT(result == VK_SUCCESS);
    NativeStereo::RegisterBuffer(buffer, size, instance.GetAllocator(), false);
    return {buffer, allocation};
}

// tiling.comp's defines for a tile mode and texel size (shared by the bit 39 from-image variant).
static std::vector<std::string> TilingDefines(const ImageInfo& info) {
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    std::vector<std::string> defines = {
        fmt::format("BITS_PER_PIXEL={}", info.num_bits),
        fmt::format("NUM_SAMPLES={}", info.num_samples),
        fmt::format("ARRAY_MODE={}", u32(info.array_mode)),
        fmt::format("MICRO_TILE_MODE={}", u32(micro_tile_mode)),
        fmt::format("MICRO_TILE_THICKNESS={}", AmdGpu::GetMicroTileThickness(info.array_mode)),
    };
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        const u32 num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        defines.emplace_back(
            fmt::format("PIPE_CONFIG={}", u32(AmdGpu::GetPipeConfig(info.tile_mode))));
        defines.emplace_back(fmt::format("BANK_WIDTH={}", AmdGpu::GetBankWidth(macro_tile_mode)));
        defines.emplace_back(fmt::format("BANK_HEIGHT={}", AmdGpu::GetBankHeight(macro_tile_mode)));
        defines.emplace_back(fmt::format("NUM_BANKS={}", num_banks));
        defines.emplace_back(fmt::format("NUM_BANK_BITS={}", std::bit_width(num_banks) - 1));
        defines.emplace_back(fmt::format(
            "TILE_SPLIT_BYTES={}", AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                              micro_tile_mode, info.num_bits)));
        defines.emplace_back(
            fmt::format("MACRO_TILE_ASPECT={}", AmdGpu::GetMacrotileAspect(macro_tile_mode)));
    }
    return defines;
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const u32 pl_id = u32(info.tile_mode) * NUM_BPPS + std::bit_width(info.num_bits) - 4;
    auto& tiling_pipelines = is_tiler ? tilers : detilers;
    if (auto pipeline = *tiling_pipelines[pl_id]; pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }

    const auto device = instance.GetDevice();
    std::vector<std::string> defines = TilingDefines(info);
    if (is_tiler) {
        defines.emplace_back(fmt::format("IS_TILER=1"));
    }

    const auto& module = Vulkan::Compile(HostShaders::TILING_COMP,
                                         vk::ShaderStageFlagBits::eCompute, device, defines);
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    for (const auto& def : defines) {
        LOG_INFO(Render_Vulkan, "#define {}", def);
    }
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    tiling_pipelines[pl_id] = std::move(pipeline);
    device.destroyShaderModule(module);
    return *tiling_pipelines[pl_id];
}

TileManager::Result TileManager::DetileImage(vk::Buffer in_buffer, u32 in_offset,
                                             const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [out_buffer, out_allocation] = GetScratchBuffer(info.guest_size);
    scheduler.DeferOperation([this, out_buffer, out_allocation, size = info.guest_size]() {
        ReleaseScratchBuffer({out_buffer, out_allocation}, size);
    });

    scheduler.EndRendering();

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, false));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    cmdbuf.dispatch(dim_x, 1, 1);
    if (NativeStereo::Enabled() && (NativeStereo::ExistingRight(in_buffer) || NativeStereo::ExistingRight(out_buffer))) {
        auto tiled = tiled_buffer_info, linear = linear_buffer_info;
        tiled.buffer = NativeStereo::RightBuffer(scheduler, tiled.buffer, false);
        linear.buffer = NativeStereo::RightBuffer(scheduler, linear.buffer, !false);
        auto writes = set_writes; writes[0].pBufferInfo = &tiled; writes[1].pBufferInfo = &linear;
        NativeStereo::Barrier(scheduler);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, writes);
        cmdbuf.dispatch(dim_x, 1, 1);
        NativeStereo::Barrier(scheduler);
    }
    return {out_buffer, 0};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u32 out_offset, u32 copy_size) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer, out_offset, copy_size);
        return;
    }
    if (FastRender::On(TileFromImage) &&
        TileImageFromImage(in_image, buffer_copies, out_buffer, out_offset, copy_size)) {
        return;
    }
    TileImageViaBuffer(in_image, buffer_copies, out_buffer, out_offset, copy_size, false);
}

void TileManager::TileImageViaBuffer(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                                     vk::Buffer out_buffer, u32 out_offset, u32 copy_size,
                                     bool zero_linear) {
    const auto& info = in_image.info;
    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [temp_buffer, temp_allocation] = GetScratchBuffer(info.guest_size);
    scheduler.DeferOperation([this, temp_buffer, temp_allocation, size = info.guest_size]() {
        ReleaseScratchBuffer({temp_buffer, temp_allocation}, size);
    });

    const auto cmdbuf = scheduler.Commands();
    if (zero_linear || Vulkan::Perf::Check()) [[unlikely]] {
        // VR_PERF_CHECK (tests only): the download writes only the image's rows, so the padding
        // positions of the linear scratch keep whatever an earlier use left there and the tiler
        // copies them into the guest buffer. Zeroed here, every resync writes the same padding
        // bytes and the resync memo's check compares exactly. Also the bit 39 verify mode's
        // reference (the from-image tiler leaves padding alone: zero in its cleared scratch).
        scheduler.EndRendering();
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &before});
        cmdbuf.fillBuffer(temp_buffer, 0, info.guest_size, 0);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &after});
    }
    in_image.Download(buffer_copies, temp_buffer, 0, copy_size);
    ScratchProfile::Gpu scratch_gpu(scheduler,30);

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, true));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    cmdbuf.dispatch(dim_x, 1, 1);
    if (NativeStereo::Enabled() && (NativeStereo::ExistingRight(temp_buffer) || NativeStereo::ExistingRight(out_buffer))) {
        auto tiled = tiled_buffer_info, linear = linear_buffer_info;
        tiled.buffer = NativeStereo::RightBuffer(scheduler, tiled.buffer, true);
        linear.buffer = NativeStereo::RightBuffer(scheduler, linear.buffer, !true);
        auto writes = set_writes; writes[0].pBufferInfo = &tiled; writes[1].pBufferInfo = &linear;
        NativeStereo::Barrier(scheduler);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, writes);
        cmdbuf.dispatch(dim_x, 1, 1);
        NativeStereo::Barrier(scheduler);
    }
}

// FAST bit 39: re-tile straight from the image. The download
// into a linear scratch buffer was ~81% of a resync's GPU time (clinic hall); the
// 3d5ebf4e direct image-to-buffer copies (vk_rasterizer.cpp IsComputeImageCopy) and the texel
// buffer resyncs (buffer_cache.cpp SynchronizeBufferFromImage) both come through here. bbport
// leaves this chain as its next GPU item (docs/parallel_gpu.md:542-548, ROADMAP.md:37): its
// IsComputeImageCopy (gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:2824-2893) is
// upstream's, with no image-sourced case beyond ours.
namespace {

// The R*_UINT view with the texel size of `bits`: texelFetch returns the raw texel bits, the same
// bytes copyImageToBuffer writes. None for 96 bits (R32G32B32_UINT need not be sampleable).
vk::Format FromImageViewFormat(u32 bits) {
    switch (bits) {
    case 8:
        return vk::Format::eR8Uint;
    case 16:
        return vk::Format::eR16Uint;
    case 32:
        return vk::Format::eR32Uint;
    case 64:
        return vk::Format::eR32G32Uint;
    case 128:
        return vk::Format::eR32G32B32A32Uint;
    default:
        return vk::Format::eUndefined;
    }
}

// tiling.comp's FromImageInfo push constants.
struct FromImagePush {
    u32 layer;
    u32 width;
    u32 height;
};

enum class FromImageFallback { None, Kind, Format, Copy, Check };

// Only what the from-image tiler reproduces exactly: a single-sample 2D colour image with one mip
// and one guest layer (two host layers when native stereo), a host format of the guest's texel
// size that a same-size uint view may reinterpret (every image is MutableFormat|ExtendedUsage,
// image.cpp), and one copy of the whole mip laid out with the guest pitch and height (what the
// download path writes into its linear scratch).
// `depth` (bit 60): instead a 32-bit float depth image (host D32Sfloat, or D32SfloatS8Uint with
// the guest's separate stencil, which the copy leaves out) and a copy of its depth aspect only:
// the tiler samples the depth aspect (always sampled usage, image.cpp ImageUsageFlags).
FromImageFallback FromImageEligibility(const Image& image,
                                       std::span<const vk::BufferImageCopy> copies, bool depth) {
    const auto& info = image.info;
    const bool aspect_ok =
        depth ? info.props.is_depth &&
                    (image.aspect_mask & vk::ImageAspectFlagBits::eDepth) &&
                    !(image.aspect_mask & ~(vk::ImageAspectFlagBits::eDepth |
                                            vk::ImageAspectFlagBits::eStencil))
              : !info.props.is_depth && !info.props.has_stencil &&
                    image.aspect_mask == vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor};
    if (!image.backing || !aspect_ok || info.props.is_block || info.props.is_volume ||
        info.num_samples != 1 || info.resources.levels != 1 || info.resources.layers != 1 ||
        info.size.depth != 1 || image.backing->num_samples != 1) {
        return FromImageFallback::Kind;
    }
    const auto& image_ci = image.backing->image.image_ci;
    if (image_ci.imageType != vk::ImageType::e2D || image_ci.mipLevels != 1 ||
        image_ci.samples != vk::SampleCountFlagBits::e1 ||
        image_ci.arrayLayers != (image.native_stereo ? 2u : 1u)) {
        return FromImageFallback::Kind;
    }
    const vk::Format format = image_ci.format;
    if (depth) {
        if (info.num_bits != 32 ||
            (format != vk::Format::eD32Sfloat && format != vk::Format::eD32SfloatS8Uint) ||
            !(image_ci.usage & vk::ImageUsageFlagBits::eSampled)) {
            return FromImageFallback::Format;
        }
    } else if (FromImageViewFormat(info.num_bits) == vk::Format::eUndefined ||
               vk::isCompressed(format) || vk::hasDepthComponent(format) ||
               vk::hasStencilComponent(format) ||
               u32{vk::blockSize(format)} * 8u != info.num_bits) {
        return FromImageFallback::Format;
    }
    if (copies.size() != 1) {
        return FromImageFallback::Copy;
    }
    const auto& mip = info.mips_layout[0];
    const auto& copy = copies[0];
    if (copy.bufferOffset != 0 || copy.bufferRowLength != mip.pitch ||
        copy.bufferImageHeight != mip.height || mip.offset != 0 ||
        copy.imageSubresource.mipLevel != 0 || copy.imageSubresource.baseArrayLayer != 0 ||
        copy.imageSubresource.layerCount != 1 || copy.imageOffset.x != 0 ||
        copy.imageOffset.y != 0 || copy.imageOffset.z != 0 || copy.imageExtent.depth != 1 ||
        copy.imageExtent.width == 0 || copy.imageExtent.height == 0 ||
        copy.imageExtent.width > mip.pitch || copy.imageExtent.height > mip.height ||
        (depth && copy.imageSubresource.aspectMask !=
                      vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth})) {
        return FromImageFallback::Copy;
    }
    if (Vulkan::Perf::Check()) {
        // VR_PERF_CHECK zero-fills the download's scratch so resync padding compares exactly;
        // the from-image tiler leaves padding bytes alone instead.
        return FromImageFallback::Check;
    }
    return FromImageFallback::None;
}

void ReportTileFromImage() {
    auto& s = g_tile_from_image_stats;
    const u64 events = s.events.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((events & 1023) != 0) {
        return;
    }
    const auto get = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    LOG_INFO(Render_Vulkan,
             "FAST tile_from_image: {} re-tiles, from image {} ({:.1f} MB); download kept: kind "
             "{} format {} copy {} perf_check {} failed {}; verified {} mismatches {}; copy "
             "chain: image-sourced writes {} uploaded again {} same layout {}",
             events, get(s.from_image), static_cast<double>(get(s.from_image_bytes)) / (1 << 20),
             get(s.fallback_kind), get(s.fallback_format), get(s.fallback_copy),
             get(s.fallback_check), get(s.fallback_failed), get(s.verified), get(s.mismatches),
             get(s.chain_writes), get(s.chain_reuploads), get(s.chain_same_layout));
}

// Bit 60's line, every 512 depth re-tiles with the bit on (about 4 s at 125 fps).
void ReportTileDepthFromImage(u64 eligible) {
    if ((eligible & 511) != 0) {
        return;
    }
    const auto& s = g_tile_from_image_stats;
    const auto get = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    LOG_INFO(Render_Vulkan,
             "FAST tile_depth_from_image: {} eligible, taken {} ({:.1f} MB); download kept: kind "
             "{} format {} copy {} perf_check {} failed {}; verified {} mismatches {}",
             eligible, get(s.depth_from_image),
             static_cast<double>(get(s.depth_from_image_bytes)) / (1 << 20),
             get(s.depth_fallback_kind), get(s.depth_fallback_format), get(s.depth_fallback_copy),
             get(s.depth_fallback_check), get(s.depth_fallback_failed), get(s.depth_verified),
             get(s.depth_mismatches));
}

} // namespace

bool TileManager::TileImageFromImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                                     vk::Buffer out_buffer, u32 out_offset, u32 copy_size) {
    auto& stats = g_tile_from_image_stats;
    const auto bump = [](std::atomic<u64>& counter) {
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    };
    // Bit 60: depth images get their own eligibility and counters. Bit off: a depth image is
    // refused with 'kind' below, exactly as before.
    const bool depth = in_image.info.props.is_depth && FastRender::On(TileDepthFromImage);
    const u64 depth_eligible = depth ? bump(stats.depth_eligible) : 0;
    bool done = false;
    switch (FromImageEligibility(in_image, buffer_copies, depth)) {
    case FromImageFallback::Kind:
        bump(depth ? stats.depth_fallback_kind : stats.fallback_kind);
        break;
    case FromImageFallback::Format:
        bump(depth ? stats.depth_fallback_format : stats.fallback_format);
        break;
    case FromImageFallback::Copy:
        bump(depth ? stats.depth_fallback_copy : stats.fallback_copy);
        break;
    case FromImageFallback::Check:
        bump(depth ? stats.depth_fallback_check : stats.fallback_check);
        break;
    case FromImageFallback::None: {
        const u32 width = buffer_copies[0].imageExtent.width;
        const u32 height = buffer_copies[0].imageExtent.height;
        done = RetileFromImage(in_image, width, height, out_buffer, out_offset, depth);
        if (!done) {
            bump(depth ? stats.depth_fallback_failed : stats.fallback_failed);
            break;
        }
        const u64 n = bump(depth ? stats.depth_from_image : stats.from_image);
        (depth ? stats.depth_from_image_bytes : stats.from_image_bytes)
            .fetch_add(u64{in_image.info.guest_size} * (in_image.native_stereo ? 2u : 1u),
                       std::memory_order_relaxed);
        if (n == 1) {
            LOG_INFO(Render_Vulkan,
                     "FAST {}: first re-tile read from the image: {:#x} {}x{} {} "
                     "({} bits, tile {}, stereo {})",
                     depth ? "tile_depth_from_image" : "tile_from_image",
                     in_image.info.guest_address, width, height,
                     vk::to_string(in_image.info.pixel_format), in_image.info.num_bits,
                     static_cast<u32>(in_image.info.tile_mode), in_image.native_stereo ? 1 : 0);
        }
        // FAST_TILE_FROM_IMAGE_VERIFY=N (colour) / FAST_TILE_DEPTH_FROM_IMAGE_VERIFY=N (depth,
        // bit 60): every Nth one is also done both ways into cleared scratch buffers and compared
        // (left eye) once the GPU is done.
        const auto period_of = [](const char* name) {
            const char* v = std::getenv(name);
            const int period = v ? std::atoi(v) : 0;
            return period > 0 ? static_cast<u64>(period) : u64{0};
        };
        static const u64 verify_period = period_of("FAST_TILE_FROM_IMAGE_VERIFY");
        static const u64 depth_verify_period = period_of("FAST_TILE_DEPTH_FROM_IMAGE_VERIFY");
        const u64 period = depth ? depth_verify_period : verify_period;
        if (period != 0 && n % period == 0) {
            VerifyFromImage(in_image, buffer_copies, copy_size, width, height, depth);
        }
        break;
    }
    }
    ReportTileFromImage();
    if (depth) {
        ReportTileDepthFromImage(depth_eligible);
    }
    return done;
}

bool TileManager::RetileFromImage(Image& in_image, u32 width, u32 height, vk::Buffer out_buffer,
                                  u32 out_offset, bool depth) {
    const auto& info = in_image.info;
    const vk::Pipeline pipeline = GetFromImagePipeline(info, depth);
    if (!pipeline) {
        return false;
    }
    // Both host layers of a native-stereo image; `layer` (push constant) picks the eye. A depth
    // image (bit 60) is viewed in its own format, depth aspect only (depth formats have no
    // compatible uint view; the stencil aspect is the guest's separate buffer).
    const vk::ImageViewUsageCreateInfo view_usage{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo view_ci{
        .pNext = &view_usage,
        .image = in_image.GetImage(),
        .viewType = vk::ImageViewType::e2DArray,
        .format = depth ? in_image.backing->image.image_ci.format
                        : FromImageViewFormat(info.num_bits),
        .subresourceRange{
            .aspectMask =
                depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = in_image.backing->image.image_ci.arrayLayers,
        },
    };
    auto view = instance.GetDevice().createImageViewUnique(view_ci);
    if (view.result != vk::Result::eSuccess) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(Render_Vulkan, "FAST tile_from_image: view of {} failed ({}); download kept",
                      vk::to_string(info.pixel_format), vk::to_string(view.result));
        }
        return false;
    }

    // As the download path's tiler (one mip, one slice).
    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = 1;
    params.num_mips = 1;
    params.mips[0] = info.mips_layout[0];
    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    // A dispatch must never be recorded inside a render pass; Transit ends one only when it
    // records a barrier. The transition also resolves a dirty reduced scene proxy first
    // (Image::GetBarriers), as the download's transition did.
    scheduler.EndRendering();
    in_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                     {});
    ScratchProfile::Gpu scratch_gpu(scheduler, 30);
    const auto cmdbuf = scheduler.Commands();
    // Earlier reads and writes of the guest bytes (either eye's copy) before the tiler's writes.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &before});
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorImageInfo image_info{
        .imageView = *view.value,
        .imageLayout = in_image.backing->state.layout,
    };
    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &image_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    const vk::PipelineLayout layout = *from_image_pl_layout;
    const u32 dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    FromImagePush push{.layer = 0, .width = width, .height = height};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
    cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                         static_cast<u32>(sizeof(push)), &push);
    cmdbuf.dispatch(dim_x, 1, 1);
    if (in_image.native_stereo || NativeStereo::ExistingRight(out_buffer)) {
        // As the download path: the right eye's layer into the right copy of the guest bytes
        // (made from the left copy when missing; RightBuffer records its own barriers). A mono
        // image gives the right copy the same bytes.
        auto right_tiled = tiled_buffer_info;
        right_tiled.buffer = NativeStereo::RightBuffer(scheduler, out_buffer, true);
        auto right_writes = set_writes;
        right_writes[0].pBufferInfo = &right_tiled;
        push.layer = in_image.native_stereo ? 1u : 0u;
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, right_writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             static_cast<u32>(sizeof(push)), &push);
        cmdbuf.dispatch(dim_x, 1, 1);
    }
    // The tiler's writes before any later use of the guest bytes (the
    // download path's tiler had none of its own).
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &after});
    scheduler.DeferOperation([retired = std::move(view.value)]() mutable { retired.reset(); });
    return true;
}

void TileManager::VerifyFromImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                                  u32 copy_size, u32 width, u32 height, bool depth) {
    // At most two comparisons in flight (each holds 2x the image in host memory until done).
    static std::atomic<u32> in_flight{0};
    const u32 size = in_image.info.guest_size;
    if (size == 0 || (size & 3u) != 0 || in_flight.load(std::memory_order_acquire) >= 2) {
        return;
    }
    const VmaAllocator allocator = instance.GetAllocator();
    const VkBufferCreateInfo readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize{size} * 2,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo readback_alloc_ci{
        .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer readback{};
    VmaAllocation readback_allocation{};
    VmaAllocationInfo readback_info{};
    if (vmaCreateBuffer(allocator, &readback_ci, &readback_alloc_ci, &readback,
                        &readback_allocation, &readback_info) != VK_SUCCESS) {
        return;
    }
    if (!readback_info.pMappedData) {
        vmaDestroyBuffer(allocator, readback, readback_allocation);
        return;
    }
    in_flight.fetch_add(1, std::memory_order_acq_rel);

    // Both outputs start from zeros: the reference's linear scratch is cleared too, so its padding
    // positions are zeros like the bytes the from-image tiler leaves alone.
    const auto from_image = GetScratchBuffer(size);
    const auto reference = GetScratchBuffer(size);
    scheduler.EndRendering();
    {
        const auto cmdbuf = scheduler.Commands();
        const vk::MemoryBarrier2 before_fill{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &before_fill});
        cmdbuf.fillBuffer(from_image.first, 0, size, 0);
        cmdbuf.fillBuffer(reference.first, 0, size, 0);
        const vk::MemoryBarrier2 after_fill{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &after_fill});
    }
    const bool compared = RetileFromImage(in_image, width, height, from_image.first, 0, depth);
    TileImageViaBuffer(in_image, buffer_copies, reference.first, 0, copy_size, true);
    scheduler.EndRendering();
    {
        const auto cmdbuf = scheduler.Commands();
        const vk::MemoryBarrier2 before_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &before_copy});
        cmdbuf.copyBuffer(from_image.first, vk::Buffer{readback}, vk::BufferCopy{0, 0, size});
        cmdbuf.copyBuffer(reference.first, vk::Buffer{readback}, vk::BufferCopy{0, size, size});
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &to_host});
    }
    scheduler.DeferOperation([this, from_image, reference, size]() {
        ReleaseScratchBuffer(from_image, size);
        ReleaseScratchBuffer(reference, size);
    });
    const VAddr address = in_image.info.guest_address;
    const u32 bits = in_image.info.num_bits;
    const void* const mapped = readback_info.pMappedData;
    scheduler.DeferOperation([allocator, readback, readback_allocation, mapped, size, compared,
                              address, width, height, bits, depth]() {
        vmaInvalidateAllocation(allocator, readback_allocation, 0, VK_WHOLE_SIZE);
        const auto* bytes = static_cast<const u8*>(mapped);
        auto& stats = g_tile_from_image_stats;
        if (compared) {
            (depth ? stats.depth_verified : stats.verified).fetch_add(1, std::memory_order_relaxed);
            if (std::memcmp(bytes, bytes + size, size) != 0) {
                u64 words = 0;
                u32 first = size;
                for (u32 i = 0; i + 4 <= size; i += 4) {
                    if (std::memcmp(bytes + i, bytes + size + i, 4) != 0) {
                        first = first == size ? i : first;
                        ++words;
                    }
                }
                const u64 n = (depth ? stats.depth_mismatches : stats.mismatches)
                                  .fetch_add(1, std::memory_order_relaxed) +
                              1;
                if (n <= 16) {
                    LOG_ERROR(Render_Vulkan,
                              "FAST {} verify: mismatch #{} at {:#x} ({}x{}, {} bits, "
                              "{} B): {} dwords differ, first at +{:#x}",
                              depth ? "tile_depth_from_image" : "tile_from_image", n, address,
                              width, height, bits, size, words, first);
                }
            }
        }
        vmaDestroyBuffer(allocator, readback, readback_allocation);
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
    });
}

vk::Pipeline TileManager::GetFromImagePipeline(const ImageInfo& info, bool depth) {
    // Bit 60's depth variants: 32 bits only (FromImageEligibility), one per tile mode.
    const u32 pl_id = depth ? u32(info.tile_mode)
                            : u32(info.tile_mode) * NUM_BPPS + std::bit_width(info.num_bits) - 4;
    if (depth ? (info.num_bits != 32 || pl_id >= from_depth_tilers.size() ||
                 from_depth_failed[pl_id])
              : (pl_id >= from_image_tilers.size() || from_image_failed[pl_id])) {
        return {};
    }
    vk::UniquePipeline& slot = depth ? from_depth_tilers[pl_id] : from_image_tilers[pl_id];
    const auto mark_failed = [&] {
        if (depth) {
            from_depth_failed[pl_id] = true;
        } else {
            from_image_failed[pl_id] = true;
        }
    };
    if (const vk::Pipeline pipeline = *slot) {
        return pipeline;
    }
    if (!from_image_pl_layout) {
        if (from_image_layout_failed || !CreateFromImageLayout()) {
            from_image_layout_failed = true;
            return {};
        }
    }

    const auto device = instance.GetDevice();
    std::vector<std::string> defines = TilingDefines(info);
    defines.emplace_back("IS_TILER=1");
    defines.emplace_back("TILE_FROM_IMAGE=1");
    if (depth) {
        defines.emplace_back("TILE_FROM_DEPTH=1");
    }
    const auto module_name =
        fmt::format("{}_{} tiler from {}", magic_enum::enum_name(info.tile_mode), info.num_bits,
                    depth ? "depth" : "image");
    const vk::ShaderModule module = Vulkan::Compile(
        HostShaders::TILING_COMP, vk::ShaderStageFlagBits::eCompute, device, defines);
    if (!module) {
        // Vulkan::Compile logged the error; this tile mode and size keep the download path.
        mark_failed();
        LOG_ERROR(Render_Vulkan, "FAST tile_from_image: compiling {} failed; download kept",
                  module_name);
        return {};
    }
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *from_image_pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        mark_failed();
        LOG_ERROR(Render_Vulkan, "FAST tile_from_image: pipeline {} failed ({}); download kept",
                  module_name, vk::to_string(result));
        return {};
    }
    slot = std::move(pipeline);
    return *slot;
}

bool TileManager::CreateFromImageLayout() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    if (desc_layout_result.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "FAST tile_from_image: descriptor set layout failed ({})",
                  vk::to_string(desc_layout_result.result));
        return false;
    }
    const vk::DescriptorSetLayout set_layout = *desc_layout_result.value;
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = static_cast<u32>(sizeof(FromImagePush)),
    };
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_range,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "FAST tile_from_image: pipeline layout failed ({})",
                  vk::to_string(layout_result));
        return false;
    }
    from_image_desc_layout = std::move(desc_layout_result.value);
    from_image_pl_layout = std::move(layout);
    return true;
}

// FAST bit 61: detile straight into the image (gpu_plan.md #6, research_gpu_copies.md R3, the
// mirror of bit 39). TextureCache::RefreshImage's full-screen uploads (3 per frame, the 'image
// upload' label, about 52 us each at the test size) detiled the guest bytes into a linear
// scratch buffer and then ran copyBufferToImage per eye; here the detiler writes the image's
// layers itself, which removes one full read plus write of the image per eye.
namespace {

// The R*_UINT storage view with the texel size of `bits`: imageStore writes the raw texel bits,
// the same bytes copyBufferToImage writes. 32, 64 and 128 bits only (tiling.comp).
vk::Format ToImageViewFormat(u32 bits) {
    switch (bits) {
    case 32:
        return vk::Format::eR32Uint;
    case 64:
        return vk::Format::eR32G32Uint;
    case 128:
        return vk::Format::eR32G32B32A32Uint;
    default:
        return vk::Format::eUndefined;
    }
}

// tiling.comp's ToImageInfo push constants.
struct ToImagePush {
    u32 layer;
    u32 width;
    u32 height;
};

enum class ToImageFallback { None, Linear, Kind, Format, Copy };

// Only what the to-image detiler reproduces exactly: a tiled single-sample 2D colour image with
// one mip and one guest layer (two host layers when native stereo) and storage usage, a host
// format of the guest's texel size that a same-size uint view may reinterpret (every image is
// MutableFormat|ExtendedUsage, image.cpp), and one copy of the whole mip laid out with the guest
// pitch and height whose texels all lie inside DetileImage's dispatch (texels past it kept stale
// scratch bytes there).
ToImageFallback ToImageEligibility(const Image& image,
                                   std::span<const vk::BufferImageCopy> copies) {
    const auto& info = image.info;
    if (!info.props.is_tiled) {
        return ToImageFallback::Linear;
    }
    if (!image.backing || info.props.is_depth || info.props.has_stencil || info.props.is_block ||
        info.props.is_volume || info.num_samples != 1 || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.size.depth != 1 ||
        image.aspect_mask != vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor} ||
        image.backing->num_samples != 1) {
        return ToImageFallback::Kind;
    }
    const auto& image_ci = image.backing->image.image_ci;
    if (image_ci.imageType != vk::ImageType::e2D || image_ci.mipLevels != 1 ||
        image_ci.samples != vk::SampleCountFlagBits::e1 ||
        image_ci.arrayLayers != (image.native_stereo ? 2u : 1u) ||
        image_ci.tiling != vk::ImageTiling::eOptimal ||
        !(image_ci.usage & vk::ImageUsageFlagBits::eStorage) ||
        !(image_ci.flags & vk::ImageCreateFlagBits::eMutableFormat)) {
        return ToImageFallback::Kind;
    }
    const vk::Format format = image_ci.format;
    if (ToImageViewFormat(info.num_bits) == vk::Format::eUndefined || vk::isCompressed(format) ||
        vk::hasDepthComponent(format) || vk::hasStencilComponent(format) ||
        u32{vk::blockSize(format)} * 8u != info.num_bits) {
        return ToImageFallback::Format;
    }
    if (copies.size() != 1) {
        return ToImageFallback::Copy;
    }
    const auto& mip = info.mips_layout[0];
    const auto& copy = copies[0];
    if (copy.bufferOffset != 0 || mip.pitch == 0 || mip.height == 0 ||
        copy.bufferRowLength != mip.pitch || copy.bufferImageHeight != mip.height ||
        mip.offset != 0 || copy.imageSubresource.mipLevel != 0 ||
        copy.imageSubresource.baseArrayLayer != 0 || copy.imageSubresource.layerCount != 1 ||
        copy.imageOffset.x != 0 || copy.imageOffset.y != 0 || copy.imageOffset.z != 0 ||
        copy.imageExtent.depth != 1 || copy.imageExtent.width == 0 ||
        copy.imageExtent.height == 0 || copy.imageExtent.width > mip.pitch ||
        copy.imageExtent.height > mip.height) {
        return ToImageFallback::Copy;
    }
    // Every texel the copy reads must have been detiled by DetileImage's dispatch, as mip 0.
    const u64 bpp = info.num_bits / 8;
    const u64 needed = u64{copy.imageExtent.height - 1} * mip.pitch + copy.imageExtent.width;
    const u64 dispatched = (u64{info.guest_size} / bpp) / 64 * 64;
    if (needed > u64{mip.size} / bpp || needed > dispatched) {
        return ToImageFallback::Copy;
    }
    return ToImageFallback::None;
}

void ReportUploadDetile() {
    auto& s = g_upload_detile_stats;
    const u64 events = s.events.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((events & 1023) != 0) {
        return;
    }
    const auto get = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    const u64 fallback = get(s.fallback_linear) + get(s.fallback_kind) + get(s.fallback_format) +
                         get(s.fallback_copy) + get(s.fallback_failed);
    LOG_INFO(Render_Vulkan,
             "FAST upload_detile: {} uploads, eligible {}, taken {} ({:.1f} MB); fallback {}: "
             "linear {} kind {} format {} copy {} failed {}; verified {} mismatches {}",
             events, get(s.taken) + get(s.fallback_failed), get(s.taken),
             static_cast<double>(get(s.taken_bytes)) / (1 << 20), fallback, get(s.fallback_linear),
             get(s.fallback_kind), get(s.fallback_format), get(s.fallback_copy),
             get(s.fallback_failed), get(s.verified), get(s.mismatches));
}

} // namespace

bool TileManager::DetileIntoImage(Image& image, std::span<const vk::BufferImageCopy> copies,
                                  vk::Buffer in_buffer, u32 in_offset) {
    auto& stats = g_upload_detile_stats;
    const auto bump = [](std::atomic<u64>& counter) {
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    };
    ToImageFallback reason = ToImageEligibility(image, copies);
    if (reason == ToImageFallback::None &&
        !ToImageFormatSupported(ToImageViewFormat(image.info.num_bits), image.info.num_bits)) {
        reason = ToImageFallback::Format;
    }
    bool done = false;
    u64 first_of_reason = 0;
    switch (reason) {
    case ToImageFallback::Linear:
        first_of_reason = bump(stats.fallback_linear);
        break;
    case ToImageFallback::Kind:
        first_of_reason = bump(stats.fallback_kind);
        break;
    case ToImageFallback::Format:
        first_of_reason = bump(stats.fallback_format);
        break;
    case ToImageFallback::Copy:
        first_of_reason = bump(stats.fallback_copy);
        break;
    case ToImageFallback::None: {
        const u32 width = copies[0].imageExtent.width;
        const u32 height = copies[0].imageExtent.height;
        done = RecordDetileIntoImage(image, width, height, in_buffer, in_offset);
        if (!done) {
            bump(stats.fallback_failed);
            break;
        }
        const u64 n = bump(stats.taken);
        stats.taken_bytes.fetch_add(u64{image.info.guest_size} * (image.native_stereo ? 2u : 1u),
                                    std::memory_order_relaxed);
        if (n == 1) {
            LOG_INFO(Render_Vulkan,
                     "FAST upload_detile: first upload detiled into the image: {:#x} {}x{} {} "
                     "({} bits, tile {}, stereo {})",
                     image.info.guest_address, width, height,
                     vk::to_string(image.info.pixel_format), image.info.num_bits,
                     static_cast<u32>(image.info.tile_mode), image.native_stereo ? 1 : 0);
        }
        // FAST_UPLOAD_DETILE_VERIFY=N: every Nth one is also detiled the old way (DetileImage into
        // a scratch buffer) and both are read back and compared per eye once the GPU is done.
        static const u64 verify_period = [] {
            const char* v = std::getenv("FAST_UPLOAD_DETILE_VERIFY");
            const int period = v ? std::atoi(v) : 0;
            return period > 0 ? static_cast<u64>(period) : u64{0};
        }();
        if (verify_period != 0 && n % verify_period == 0) {
            VerifyDetileIntoImage(image, copies[0], in_buffer, in_offset);
        }
        break;
    }
    }
    if (first_of_reason == 1) {
        // The upload probe (research_gpu_copies.md U1): the first image of each fallback reason.
        LOG_INFO(Render_Vulkan,
                 "FAST upload_detile: first fallback ({}): {:#x} {}x{} {} ({} bits, tile {}, "
                 "tiled {}, mips {}, layers {}, copies {}, stereo {})",
                 static_cast<u32>(reason), image.info.guest_address, image.info.size.width,
                 image.info.size.height, vk::to_string(image.info.pixel_format),
                 image.info.num_bits, static_cast<u32>(image.info.tile_mode),
                 image.info.props.is_tiled ? 1 : 0, image.info.resources.levels,
                 image.info.resources.layers, copies.size(), image.native_stereo ? 1 : 0);
    }
    ReportUploadDetile();
    return done;
}

bool TileManager::RecordDetileIntoImage(Image& image, u32 width, u32 height, vk::Buffer in_buffer,
                                        u32 in_offset) {
    const auto& info = image.info;
    const vk::Pipeline pipeline = GetToImagePipeline(info);
    if (!pipeline) {
        return false;
    }
    // Both host layers of a native-stereo image; `layer` (push constant) picks the eye.
    const vk::ImageViewUsageCreateInfo view_usage{.usage = vk::ImageUsageFlagBits::eStorage};
    const vk::ImageViewCreateInfo view_ci{
        .pNext = &view_usage,
        .image = image.GetImage(),
        .viewType = vk::ImageViewType::e2DArray,
        .format = ToImageViewFormat(info.num_bits),
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = image.backing->image.image_ci.arrayLayers,
        },
    };
    auto view = instance.GetDevice().createImageViewUnique(view_ci);
    if (view.result != vk::Result::eSuccess) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(Render_Vulkan, "FAST upload_detile: view of {} failed ({}); copy kept",
                      vk::to_string(info.pixel_format), vk::to_string(view.result));
        }
        return false;
    }

    // As DetileImage's (one mip, one slice; the eligibility check excludes block formats).
    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = 1;
    params.num_mips = 1;
    params.mips[0] = info.mips_layout[0];
    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    // A dispatch must never be recorded inside a render pass.
    scheduler.EndRendering();
    ScratchProfile::Gpu scratch_gpu(scheduler, 20);
    // As Upload's transition to TransferDst: the image's earlier accesses before the writes (it
    // also resolves a dirty reduced scene proxy and moves the resync memo's write counter). The
    // guest bytes' earlier writes (either eye's copy) before the detiler reads them.
    const auto image_barriers =
        image.GetBarriers(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
                          vk::PipelineStageFlagBits2::eComputeShader, {});
    const auto cmdbuf = scheduler.Commands();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &before,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorImageInfo image_info{
        .imageView = *view.value,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &image_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    const vk::PipelineLayout layout = *to_image_pl_layout;
    // DetileImage's dispatch size (the eligibility check put every copied texel inside it).
    const u32 dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    ToImagePush push{.layer = 0, .width = width, .height = height};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
    cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                         static_cast<u32>(sizeof(push)), &push);
    cmdbuf.dispatch(dim_x, 1, 1);
    if (image.native_stereo) {
        // As DetileImage + Upload: layer 1 gets the right copy of the guest bytes when there is
        // one, else the left bytes again. The eyes read only and write different layers, so no
        // barrier between them.
        const vk::Buffer right = NativeStereo::ExistingRight(in_buffer);
        auto right_tiled = tiled_buffer_info;
        right_tiled.buffer = right ? right : in_buffer;
        auto right_writes = set_writes;
        right_writes[0].pBufferInfo = &right_tiled;
        push.layer = 1;
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, right_writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             static_cast<u32>(sizeof(push)), &push);
        cmdbuf.dispatch(dim_x, 1, 1);
    }
    // Upload's final state: Transit(eGeneral, ShaderRead | TransferRead), with Transit's stage for
    // that mask. The extra global dependency keeps what the old path's copy and post-barrier
    // chained: the detiler's work before any later command (a later transfer write of the guest
    // bytes it read, for one).
    const auto after_barriers = image.GetBarriers(
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
        vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader, {});
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &after,
        .imageMemoryBarrierCount = static_cast<u32>(after_barriers.size()),
        .pImageMemoryBarriers = after_barriers.data(),
    });
    scheduler.DeferOperation([retired = std::move(view.value)]() mutable { retired.reset(); });
    return true;
}

void TileManager::VerifyDetileIntoImage(Image& image, const vk::BufferImageCopy& copy,
                                        vk::Buffer in_buffer, u32 in_offset) {
    // One comparison in flight (it holds 2x the image per eye in host memory until done).
    static std::atomic<u32> in_flight{0};
    const auto& info = image.info;
    const u32 bpp = info.num_bits / 8;
    const u64 row_bytes = u64{copy.bufferRowLength} * bpp;
    const u64 region = row_bytes * copy.bufferImageHeight;
    const u32 eyes = image.native_stereo ? 2u : 1u;
    if (region == 0 || region > info.guest_size || in_flight.load(std::memory_order_acquire) != 0) {
        return;
    }
    const VmaAllocator allocator = instance.GetAllocator();
    const VkBufferCreateInfo readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize{region} * 2 * eyes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo readback_alloc_ci{
        .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer readback{};
    VmaAllocation readback_allocation{};
    VmaAllocationInfo readback_info{};
    if (vmaCreateBuffer(allocator, &readback_ci, &readback_alloc_ci, &readback,
                        &readback_allocation, &readback_info) != VK_SUCCESS) {
        return;
    }
    if (!readback_info.pMappedData) {
        vmaDestroyBuffer(allocator, readback, readback_allocation);
        return;
    }
    in_flight.fetch_add(1, std::memory_order_acq_rel);

    // The reference per eye: the old path's detile into a scratch buffer (DetileImage defers the
    // scratch's release itself). Layer 1's guest bytes as RecordDetileIntoImage chose them.
    const vk::Buffer right_in = NativeStereo::ExistingRight(in_buffer);
    std::array<vk::Buffer, 2> reference{};
    for (u32 eye = 0; eye < eyes; ++eye) {
        const vk::Buffer src = eye == 0 ? in_buffer : (right_in ? right_in : in_buffer);
        reference[eye] = DetileImage(src, in_offset, info).first;
    }
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    scheduler.EndRendering();
    {
        const auto cmdbuf = scheduler.Commands();
        const vk::MemoryBarrier2 before_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &before_copy});
        for (u32 eye = 0; eye < eyes; ++eye) {
            const VkDeviceSize base = VkDeviceSize{region} * 2 * eye;
            vk::BufferImageCopy image_copy = copy;
            image_copy.bufferOffset = base;
            image_copy.imageSubresource.baseArrayLayer = eye;
            image_copy.imageSubresource.layerCount = 1;
            cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                     vk::Buffer{readback}, image_copy);
            cmdbuf.copyBuffer(reference[eye], vk::Buffer{readback},
                              vk::BufferCopy{0, base + region, region});
        }
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &to_host});
    }
    // Back to Upload's final state.
    image.Transit(vk::ImageLayout::eGeneral,
                  vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});

    const VAddr address = info.guest_address;
    const u32 width = copy.imageExtent.width;
    const u32 height = copy.imageExtent.height;
    const u32 bits = info.num_bits;
    const void* const mapped = readback_info.pMappedData;
    scheduler.DeferOperation([allocator, readback, readback_allocation, mapped, region, row_bytes,
                              eyes, width, height, bpp, bits, address]() {
        vmaInvalidateAllocation(allocator, readback_allocation, 0, VK_WHOLE_SIZE);
        const auto* bytes = static_cast<const u8*>(mapped);
        auto& stats = g_upload_detile_stats;
        for (u32 eye = 0; eye < eyes; ++eye) {
            const u8* actual = bytes + region * 2 * eye;
            const u8* expected = actual + region;
            // Only the copied texels: the image copy leaves the row padding of its half unwritten.
            u64 rows_differing = 0;
            u32 first_row = height;
            for (u32 y = 0; y < height; ++y) {
                if (std::memcmp(actual + y * row_bytes, expected + y * row_bytes,
                                u64{width} * bpp) != 0) {
                    first_row = first_row == height ? y : first_row;
                    ++rows_differing;
                }
            }
            stats.verified.fetch_add(1, std::memory_order_relaxed);
            if (rows_differing != 0) {
                const u64 n = stats.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 16) {
                    LOG_ERROR(Render_Vulkan,
                              "FAST upload_detile verify: mismatch #{} at {:#x} eye {} ({}x{}, {} "
                              "bits): {} rows differ, first {}",
                              n, address, eye, width, height, bits, rows_differing, first_row);
                }
            }
        }
        vmaDestroyBuffer(allocator, readback, readback_allocation);
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
    });
}

bool TileManager::ToImageFormatSupported(vk::Format view_format, u32 bits) {
    if (view_format == vk::Format::eUndefined || bits < 8) {
        return false;
    }
    const u32 index = static_cast<u32>(std::bit_width(bits)) - 4;
    if (index >= to_image_format_support.size()) {
        return false;
    }
    u8& support = to_image_format_support[index];
    if (support == 0) {
        const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
        bool ok = static_cast<bool>(physical.getFormatProperties(view_format).optimalTilingFeatures &
                                    vk::FormatFeatureFlagBits::eStorageImage);
        // rg32ui is an extended storage format (enabled at device creation when supported).
        if (ok && bits == 64) {
            ok = physical.getFeatures().shaderStorageImageExtendedFormats == VK_TRUE;
        }
        support = ok ? 1 : 2;
    }
    return support == 1;
}

vk::Pipeline TileManager::GetToImagePipeline(const ImageInfo& info) {
    const u32 pl_id = u32(info.tile_mode) * NUM_BPPS + std::bit_width(info.num_bits) - 4;
    if (pl_id >= to_image_detilers.size() || to_image_failed[pl_id]) {
        return {};
    }
    if (const vk::Pipeline pipeline = *to_image_detilers[pl_id]) {
        return pipeline;
    }
    if (!to_image_pl_layout) {
        if (to_image_layout_failed || !CreateToImageLayout()) {
            to_image_layout_failed = true;
            return {};
        }
    }

    const auto device = instance.GetDevice();
    std::vector<std::string> defines = TilingDefines(info);
    defines.emplace_back("DETILE_TO_IMAGE=1");
    const auto module_name = fmt::format("{}_{} detiler to image",
                                         magic_enum::enum_name(info.tile_mode), info.num_bits);
    const vk::ShaderModule module = Vulkan::Compile(
        HostShaders::TILING_COMP, vk::ShaderStageFlagBits::eCompute, device, defines);
    if (!module) {
        // Vulkan::Compile logged the error; this tile mode and size keep the copy path.
        to_image_failed[pl_id] = true;
        LOG_ERROR(Render_Vulkan, "FAST upload_detile: compiling {} failed; copy kept",
                  module_name);
        return {};
    }
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *to_image_pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        to_image_failed[pl_id] = true;
        LOG_ERROR(Render_Vulkan, "FAST upload_detile: pipeline {} failed ({}); copy kept",
                  module_name, vk::to_string(result));
        return {};
    }
    to_image_detilers[pl_id] = std::move(pipeline);
    return *to_image_detilers[pl_id];
}

bool TileManager::CreateToImageLayout() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    if (desc_layout_result.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "FAST upload_detile: descriptor set layout failed ({})",
                  vk::to_string(desc_layout_result.result));
        return false;
    }
    const vk::DescriptorSetLayout set_layout = *desc_layout_result.value;
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = static_cast<u32>(sizeof(ToImagePush)),
    };
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_range,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "FAST upload_detile: pipeline layout failed ({})",
                  vk::to_string(layout_result));
        return false;
    }
    to_image_desc_layout = std::move(desc_layout_result.value);
    to_image_pl_layout = std::move(layout);
    return true;
}

u64 TileManager::TiledLayoutKey(const ImageInfo& info) noexcept {
    u64 key = 0xcbf29ce484222325ull;
    const auto mix = [&key](u64 value) { key = (key ^ value) * 0x100000001b3ull; };
    mix(static_cast<u64>(info.tile_mode));
    mix(static_cast<u64>(info.array_mode));
    mix(info.num_bits);
    mix(info.pitch);
    mix(info.size.width);
    mix(info.size.height);
    mix(info.guest_size);
    mix(info.bank_swizzle);
    mix(info.alt_tile ? 1u : 0u);
    mix(info.props.is_block ? 1u : 0u);
    return key;
}

void TileManager::NoteImageRefresh(const Image& image) {
    u64 layout = 0;
    if (!g_image_sourced_writes.Take(image.info.guest_address, layout)) {
        return;
    }
    auto& stats = g_tile_from_image_stats;
    const u64 n = stats.chain_reuploads.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool same = layout == TiledLayoutKey(image.info);
    if (same) {
        stats.chain_same_layout.fetch_add(1, std::memory_order_relaxed);
    }
    if (n <= 8) {
        LOG_INFO(Render_Vulkan,
                 "FAST tile_from_image: copy chain #{}: image-sourced bytes at {:#x} uploaded "
                 "into {}x{} {} ({} bits, tile {}): {}",
                 n, image.info.guest_address, image.info.size.width, image.info.size.height,
                 vk::to_string(image.info.pixel_format), image.info.num_bits,
                 static_cast<u32>(image.info.tile_mode),
                 same ? "the source's tiled layout" : "another layout");
    }
}

} // namespace VideoCore
