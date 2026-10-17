#include "../../../scratchpad/dual-eye-perf/profile_scopes.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>
#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "common/assert.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_transfer_eye_barriers.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/renderer_vulkan/vr_scene_proxy.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance* instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_info, &unsafe_image,
                                     &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
}

Image::Image(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
             BlitHelper& blit_helper_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_)
    : instance{&instance_}, scheduler{&scheduler_}, blit_helper{&blit_helper_},
      slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    // Here we force `eExtendedUsage` as don't know all image usage cases beforehand. In normal case
    // the texture cache should re-create the resource with the usage requested
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance->Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance->IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance->GetSupportedFormat(info.pixel_format, format_features);
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const auto image_format_properties =
        instance->GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    native_stereo = NativeStereo::Enabled() && ConvertImageType(info.type) == vk::ImageType::e2D && info.resources.layers == 1 && !info.props.is_block && !NativeStereo::SharedShadowAtlas(info);
    if (NativeStereo::SharedShadowAtlas(info)) {
        LOG_INFO(Render_Vulkan, "VR native shared shadow: one 4096x4096 D32 layer, address {:#x}", info.guest_address);
    }
    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = native_stereo ? 2u : info.resources.layers,
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    backing->image.Create(image_ci);

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        SetBackingSamples(info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (FastRender::CacheStatsEnabled()) {
        const u64 steps =
            it == view_infos.end() ? view_infos.size() : std::distance(view_infos.begin(), it) + 1;
        FastRender::g_counters.view_calls.fetch_add(1, std::memory_order_relaxed);
        FastRender::g_counters.view_steps.fetch_add(steps, std::memory_order_relaxed);
        FastRender::g_counters.view_misses.fetch_add(it == view_infos.end(),
                                                     std::memory_order_relaxed);
    }
    if (it != view_infos.end()) {
        const auto view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(*instance, view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

// The read-only accesses of vr_perf_logic.h, as the Vulkan headers define them.
static_assert(VideoCore::PerfLogic::ReadOnlyAccess ==
              static_cast<VkAccessFlags2>(
                  vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eIndexRead |
                  vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eUniformRead |
                  vk::AccessFlagBits2::eInputAttachmentRead | vk::AccessFlagBits2::eShaderRead |
                  vk::AccessFlagBits2::eColorAttachmentRead |
                  vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                  vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eHostRead |
                  vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eShaderSampledRead |
                  vk::AccessFlagBits2::eShaderStorageRead));

Image::Barriers Image::GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                                   vk::PipelineStageFlags2 dst_stage,
                                   std::optional<SubresourceRange> subres_range) {
    if (scene_proxy) scene_proxy->NativeAccess(*this, dst_mask);
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    // The resync memo (vr_perf.h): a transition that may write this image moves its counter (the
    // write itself follows on the GPU), whether or not it needs a barrier.
    {
        bool from_undefined = last_state.layout == vk::ImageLayout::eUndefined;
        for (const auto& sub : subresource_states) {
            from_undefined |= sub.layout == vk::ImageLayout::eUndefined;
        }
        if (PerfLogic::MayWrite(static_cast<VkAccessFlags2>(dst_mask), from_undefined)) {
            ++perf_write_seq;
            perf_write_draw = Vulkan::Perf::DrawSerial();
            if (Vulkan::Perf::Probe()) [[unlikely]] {
                perf_write_ctx = Vulkan::Perf::Context();
            }
        }
    }

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    Barriers barriers;
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return {};
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;

    if (native_stereo) for (auto& b : barriers) if (b.subresourceRange.layerCount == 1) b.subresourceRange.layerCount = 2;
    return barriers;
}

void Image::Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                    std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf /*= {}*/) {
    // Adjust pipeline stage
    const vk::PipelineStageFlags2 dst_pl_stage =
        (dst_mask == vk::AccessFlagBits2::eTransferRead ||
         dst_mask == vk::AccessFlagBits2::eTransferWrite)
            ? vk::PipelineStageFlagBits2::eTransfer
            : vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;

    const auto barriers = GetBarriers(dst_layout, dst_mask, dst_pl_stage, range);
    if (barriers.empty()) {
        return;
    }

    if (!cmdbuf) {
        // When using external cmdbuf you are responsible for ending rp.
        scheduler->EndRendering();
        scheduler->Commands().pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });
        return;
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void Image::Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer,
                   u64 offset) {
    SetBackingSamples(info.num_samples, false);
    scheduler->EndRendering();
    ScratchProfile::Gpu scratch_gpu(*scheduler,20);

    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eCopy, {});
    const auto cmdbuf = scheduler->Commands();
    // FAST bit 63 (vk_transfer_eye_barriers.h): the pre/post barriers also name the right source,
    // and the global barrier between the eyes goes. Layers 0 and 1 are disjoint subresources and
    // image_barriers already covers both (GetBarriers widens native stereo transitions).
    if (native_stereo && Vulkan::TransferEyeBarriers::On()) {
        using namespace Vulkan::TransferEyeBarriers;
        const auto right = NativeStereo::ExistingRight(buffer);
        if (right == buffer) {
            NoteFallback(Fallback::Alias);
        } else {
            std::array<vk::BufferMemoryBarrier2, 2> pre{pre_barrier, pre_barrier};
            std::array<vk::BufferMemoryBarrier2, 2> post{post_barrier, post_barrier};
            pre[1].buffer = right;
            post[1].buffer = right;
            const u32 count = right ? 2u : 1u;
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = count,
                .pBufferMemoryBarriers = pre.data(),
                .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
                .pImageMemoryBarriers = image_barriers.data(),
            });
            cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                                     upload_copies);
            std::vector<vk::BufferImageCopy> copies(upload_copies.begin(), upload_copies.end());
            for (auto& c : copies) c.imageSubresource.baseArrayLayer = 1;
            cmdbuf.copyBufferToImage(right ? right : buffer, GetImage(),
                                     vk::ImageLayout::eTransferDstOptimal, copies);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = count,
                .pBufferMemoryBarriers = post.data(),
            });
            NoteTaken(Site::Upload);
            Transit(vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
            flags &= ~ImageFlagBits::Dirty;
            return;
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             upload_copies);
    if (native_stereo) {
        auto right = NativeStereo::ExistingRight(buffer);
        std::vector<vk::BufferImageCopy> copies(upload_copies.begin(), upload_copies.end());
        for (auto& c : copies) c.imageSubresource.baseArrayLayer = 1;
        NativeStereo::Barrier(*scheduler);
        cmdbuf.copyBufferToImage(right ? right : buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal, copies);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
    flags &= ~ImageFlagBits::Dirty;
}

void Image::Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                     u64 offset, u64 download_size) {
    SetBackingSamples(info.num_samples);
    scheduler->EndRendering();
    ScratchProfile::Gpu scratch_gpu(*scheduler,21);

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    static const bool pair_download_barriers = std::getenv("VR_PAIR_DOWNLOAD_BARRIERS") != nullptr;
    // FAST bit 63 (vk_transfer_eye_barriers.h) takes the paired shape below: one barrier pair
    // naming both eyes' destinations, layer 0 -> left buffer and layer 1 -> right twin.
    const bool pair_bit = native_stereo && !pair_download_barriers &&
                          Vulkan::TransferEyeBarriers::On();
    if (native_stereo && (pair_download_barriers || pair_bit)) {
        // Establish right backing before either eye copy. Its initialization preserves
        // all untouched bytes; each download range is then overwritten by its own eye.
        const auto right = NativeStereo::RightBuffer(*scheduler, buffer, true);
        ASSERT(right && right != buffer);
        std::array<vk::BufferMemoryBarrier2, 2> before{pre_barrier, pre_barrier};
        std::array<vk::BufferMemoryBarrier2, 2> after{post_barrier, post_barrier};
        before[1].buffer = right;
        after[1].buffer = right;
        for (auto& barrier : before) {
            barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        }
        for (auto& barrier : after) {
            barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        }
        const auto transitions = GetBarriers(vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eCopy, {});
        const auto command = scheduler->Commands();
        command.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = before.data(),
            .imageMemoryBarrierCount = static_cast<u32>(transitions.size()),
            .pImageMemoryBarriers = transitions.data()});
        command.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer, download_copies);
        std::vector<vk::BufferImageCopy> right_copies(download_copies.begin(), download_copies.end());
        for (auto& copy : right_copies) copy.imageSubresource.baseArrayLayer = 1;
        command.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, right, right_copies);
        command.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = after.data()});
        if (pair_bit) {
            Vulkan::TransferEyeBarriers::NoteTaken(Vulkan::TransferEyeBarriers::Site::Download);
        }
        return;
    }
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eCopy, {});
    auto cmdbuf = scheduler->Commands();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             download_copies);
    if (native_stereo) {
        auto right = NativeStereo::RightBuffer(*scheduler, buffer, true);
        std::vector<vk::BufferImageCopy> copies(download_copies.begin(), download_copies.end());
        for (auto& c : copies) c.imageSubresource.baseArrayLayer = 1;
        NativeStereo::Barrier(*scheduler);
        cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, right, copies);
        NativeStereo::Barrier(*scheduler);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

static std::pair<u32, u32> SanitizeCopyLayers(const ImageInfo& src_info, const ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

void Image::CopyImage(Image& src_image) {
    const auto& src_info = src_image.info;

    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src_info.pixel_format != info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src_info.pixel_format), vk::to_string(info.pixel_format));
    }

    const u32 base_width = src_info.size.width;
    const u32 base_height = src_info.size.height;
    const u32 base_depth =
        info.type == AmdGpu::ImageType::Color3D ? info.size.depth : src_info.size.depth;

    // Match sample count before copying
    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect =
        src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const vk::ImageAspectFlags dst_aspect = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src_info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src_info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d);

        vk::ImageCopy region{};

        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;

        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler->EndRendering();
    ScratchProfile::Gpu scratch_gpu(*scheduler,22);

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->Commands();

    if (native_stereo) {
        const auto count = regions.size();
        for (size_t i=0; i<count; ++i) {
            auto r = regions[i]; r.dstSubresource.baseArrayLayer = 1;
            if (src_image.native_stereo) r.srcSubresource.baseArrayLayer = 1;
            regions.push_back(r);
        }
    }
    if (!regions.empty()) {
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                         backing->state.layout, regions);
    }

    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}
void Image::CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset) {
    const auto& src_info = src_image.info;
    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);
    const u32 num_layers = std::min(src_info.resources.layers, info.resources.layers);
    ASSERT(src_info.resources.layers == info.resources.layers || num_mips == 1);

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto mip_w = std::max(src_info.size.width >> mip, 1u);
        const auto mip_h = std::max(src_info.size.height >> mip, 1u);
        const auto mip_d = std::max(src_info.size.depth >> mip, 1u);

        buffer_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_w, mip_h, mip_d},
        });
    }

    const vk::BufferMemoryBarrier2 pre_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    const vk::BufferMemoryBarrier2 post_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    scheduler->EndRendering();
    ScratchProfile::Gpu scratch_gpu(*scheduler,23);
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->Commands();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_copy_barrier,
    });

    cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             buffer_copies);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_copy_barrier,
    });

    for (auto& copy : buffer_copies) {
        copy.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    }

    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             buffer_copies);
    if (native_stereo) {
        NativeStereo::Barrier(*scheduler);
        auto copies = buffer_copies;
        for (auto& c : copies) { c.imageSubresource.baseArrayLayer = src_image.native_stereo ? 1 : 0; c.imageSubresource.aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil; }
        cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer, copies);
        NativeStereo::Barrier(*scheduler);
        for (auto& c : copies) { c.imageSubresource.baseArrayLayer = 1; c.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil; }
        cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal, copies);
    }
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::CopyMip(Image& src_image, u32 mip, u32 slice) {
    const auto& src_info = src_image.info;

    const auto dst_dim = info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src_info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == (src_info.size.width >> src_dim));
    ASSERT(mip_block_h == (src_info.size.height >> src_dim));
    ASSERT(mip_block_p == (src_info.pitch >> src_dim));

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, src_info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src_info.size.width, src_info.size.height, src_info.size.depth},
    };

    SetBackingSamples(info.num_samples);
    src_image.SetBackingSamples(src_info.num_samples);

    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    const auto cmdbuf = scheduler->Commands();
    cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                     backing->state.layout, image_copy);
    if (native_stereo) {
        auto right = image_copy; right.dstSubresource.baseArrayLayer = 1;
        if (src_image.native_stereo) right.srcSubresource.baseArrayLayer = 1;
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(), backing->state.layout, right);
    }
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                    const VideoCore::SubresourceRange& mrt1_range) {
    SetBackingSamples(1, false);
    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      mrt0_range);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, mrt1_range);

    auto [src_layers, dst_layers] = SanitizeCopyLayers(src_image.info, info, 1);
    if (native_stereo && src_image.native_stereo) src_layers = dst_layers = 2;
    ASSERT_MSG(!native_stereo || src_image.native_stereo, "Native stereo: unsupported mixed resolve");
    if (src_image.backing->num_samples == 1) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->Commands().copyImage(src_image.GetImage(),
                                             vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                             vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->Commands().resolveImage(src_image.GetImage(),
                                                vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                                vk::ImageLayout::eTransferDstOptimal, region);
    }

    flags |= VideoCore::ImageFlagBits::GpuModified;
    flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Image::Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range) {
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = native_stereo ? 2u : range.extent.layers,
    };
    scheduler->EndRendering();
    ScratchProfile::Gpu scratch_gpu(*scheduler,24);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto cmdbuf = scheduler->Commands();
    cmdbuf.clearColorImage(GetImage(), vk::ImageLayout::eTransferDstOptimal, clear_value.color,
                           vk_range);
}

void Image::SetBackingSamples(u32 num_samples, bool copy_backing) {
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!info.props.is_depth, "Swapping samples is only valid for color images");
    BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples, &BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = LiverpoolToVK::NumSamples(num_samples, supported_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    ASSERT_MSG(!native_stereo || !copy_backing, "Native stereo: sample-count conversion is not yet paired");
    if (copy_backing) {
        scheduler->EndRendering();
        ASSERT(info.resources.levels == 1 && info.resources.layers == 1);

        // Transition current backing to shader read layout
        auto barriers =
            GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, std::nullopt);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
        });
        const auto cmdbuf = scheduler->Commands();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

} // namespace VideoCore
