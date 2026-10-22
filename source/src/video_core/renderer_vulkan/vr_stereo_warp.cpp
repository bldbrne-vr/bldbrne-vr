#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/vr_dibr_prep_comp.h"
#include "video_core/host_shaders/vr_dibr_warp_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

/// Mirrors PrepConstants in vr_dibr_prep.comp (std430 push constants).
struct PrepPush {
    s32 size[2];
    s32 depth_size[2];
    VrStereo::DisparityParams disparity;
    VrStereo::ProxyParams proxy;
    float write_w; ///< > 0: also the true 1/z and its tile range (the late ring)
    float stable_on; ///< > 0: aa_mode=taa's stable depth stands in for the game's (bindings 4, 5)
};
static_assert(sizeof(VrStereo::DisparityParams) == 9 * sizeof(float));
static_assert(sizeof(VrStereo::ProxyParams) == 8 * sizeof(float));
static_assert(offsetof(PrepPush, disparity) == 16);
static_assert(offsetof(PrepPush, proxy) == 52);
static_assert(sizeof(PrepPush) == 92); // within the guaranteed 128 bytes of push constants

using VrStereo::WarpPush;
using namespace VrStereo; // the WarpFlag bits
static_assert(offsetof(WarpPush, dst) == 32);
static_assert(offsetof(WarpPush, src_size) == 64);
static_assert(offsetof(WarpPush, flags) == 88);
static_assert(offsetof(WarpPush, knee_px) == 96);
static_assert(offsetof(WarpPush, k_px) == 120);
static_assert(offsetof(WarpPush, edge_k) == 124);

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceLayers ColorLayers{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

vk::Format SrgbTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eB8G8R8A8Unorm:
        return vk::Format::eB8G8R8A8Srgb;
    case vk::Format::eA8B8G8R8UnormPack32:
        return vk::Format::eA8B8G8R8SrgbPack32;
    default:
        return vk::Format::eUndefined;
    }
}

vk::Format UnormTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eA8B8G8R8SrgbPack32:
        return vk::Format::eA8B8G8R8UnormPack32;
    default:
        return format;
    }
}

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

vk::UniquePipeline MakeComputePipeline(vk::Device device, vk::PipelineLayout layout,
                                       std::string_view code, const char* name,
                                       vk::UniquePipeline* neutral = nullptr) {
    const vk::ShaderModule module = Compile(code, vk::ShaderStageFlagBits::eCompute, device);
    ASSERT_MSG(module, "Failed to compile {}", name);
    SetObjectName(device, module, std::string_view{name});
    const vk::SpecializationMapEntry color_entry{.constantID = 0, .offset = 0, .size = sizeof(u32)};
    u32 color_enabled = 1;
    const vk::SpecializationInfo color_specialization{
        .mapEntryCount = 1,
        .pMapEntries = &color_entry,
        .dataSize = sizeof(color_enabled),
        .pData = &color_enabled,
    };
    const vk::ComputePipelineCreateInfo pipeline_ci{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = neutral ? &color_specialization : nullptr,
        },
        .layout = layout,
    };
    auto pipeline = Check<"create vr dibr compute pipeline">(
        device.createComputePipelineUnique({}, pipeline_ci));
    SetObjectName(device, *pipeline, std::string_view{name});
    if (neutral) {
        color_enabled = 0;
        *neutral = Check<"create neutral vr dibr compute pipeline">(
            device.createComputePipelineUnique({}, pipeline_ci));
        SetObjectName(device, **neutral, "{} (neutral)", name);
    }
    device.destroyShaderModule(module);
    return pipeline;
}

/// Destination stage/access for an image left in `layout` after the warp.
std::pair<vk::PipelineStageFlags2, vk::AccessFlags2> ConsumerOf(vk::ImageLayout layout) {
    switch (layout) {
    case vk::ImageLayout::eTransferSrcOptimal:
        return {vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead};
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return {vk::PipelineStageFlagBits2::eFragmentShader |
                    vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderSampledRead};
    default:
        return {vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    }
}

} // Anonymous namespace

VrStereoWarp::VrStereoWarp(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()},
      allocator{instance_.GetAllocator()} {
    // ---- prep: depth (sampled) -> disparity (storage) + tiles (storage) + 1/z (storage) ----
    const std::array<vk::DescriptorSetLayoutBinding, 6> prep_bindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 4,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 5,
            .descriptorType = vk::DescriptorType::eSampledImage,
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
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    prep_set_layout =
        Check<"create vr dibr prep descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(prep_bindings.size()),
            .pBindings = prep_bindings.data(),
        }));
    const vk::PushConstantRange prep_push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PrepPush),
    };
    prep_layout = Check<"create vr dibr prep pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &prep_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &prep_push,
    }));
    SetObjectName(device, *prep_layout, "vr dibr prep pipeline layout");
    prep_pipeline = MakeComputePipeline(device, *prep_layout, HostShaders::VR_DIBR_PREP_COMP,
                                        "vr_dibr_prep.comp");

    // ---- warp: disparity, tiles, scene, hud (sampled) -> eye0, eye1 (storage), depth; 7: the
    // in-frame items' coverage (sampled) ----
    std::array<vk::DescriptorSetLayoutBinding, 8> warp_bindings{};
    for (u32 i = 0; i < warp_bindings.size(); ++i) {
        warp_bindings[i] = {
            .binding = i,
            .descriptorType = i < 4 || i == 7 ? vk::DescriptorType::eSampledImage
                              : i < 6         ? vk::DescriptorType::eStorageImage
                                              : vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    warp_set_layout =
        Check<"create vr dibr warp descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(warp_bindings.size()),
            .pBindings = warp_bindings.data(),
        }));
    const vk::PushConstantRange warp_push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(WarpPush),
    };
    warp_layout = Check<"create vr dibr warp pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &warp_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &warp_push,
    }));
    SetObjectName(device, *warp_layout, "vr dibr warp pipeline layout");
    warp_pipeline = MakeComputePipeline(device, *warp_layout, HostShaders::VR_DIBR_WARP_COMP,
                                        "vr_dibr_warp.comp", &warp_neutral_pipeline);
}

// The owner must make sure the GPU no longer uses the owned images (e.g. WaitIdle).
VrStereoWarp::~VrStereoWarp() {
    if (depth_buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(depth_buffer), depth_allocation);
    }
}

void VrStereoWarp::EnsureDepthBuffer(vk::DeviceSize size) {
    size = std::max<vk::DeviceSize>(size, 256);
    if (depth_buffer && depth_capacity >= size) {
        return;
    }
    if (depth_buffer) {
        // The last frames' warps and copies may still use it.
        scheduler.DeferOperation([allocator = allocator, buffer = depth_buffer,
                                  allocation = depth_allocation] {
            vmaDestroyBuffer(allocator, static_cast<VkBuffer>(buffer), allocation);
        });
        depth_buffer = vk::Buffer{};
    }
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    ASSERT_MSG(vmaCreateBuffer(allocator, &info, &alloc, &buffer, &depth_allocation, nullptr) ==
                   VK_SUCCESS,
               "Failed to allocate the VR DIBR depth buffer");
    depth_buffer = vk::Buffer{buffer};
    depth_capacity = size;
    SetObjectName(device, depth_buffer, "VR DIBR eye depth {} bytes", size);
}

void VrStereoWarp::EnableTiming(const Instance& instance) {
    if (query_pool) {
        return;
    }
    query_pool = Check<"create vr dibr query pool">(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = TimingSlots * 4,
    }));
    timestamp_period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
}

void VrStereoWarp::Timestamp(vk::CommandBuffer cmdbuf, u32 query) {
    if (!query_pool) {
        return;
    }
    if (timing_written == 0) {
        // First timestamp of this frame: the slot's previous results are read or dropped.
        timing_pending[timing_slot] = false;
        cmdbuf.resetQueryPool(*query_pool, timing_slot * 4, 4);
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *query_pool,
                           timing_slot * 4 + query);
    timing_written |= 1u << query;
}

void VrStereoWarp::CollectTimes() {
    for (u32 slot = 0; slot < TimingSlots; ++slot) {
        if (!timing_pending[slot]) {
            continue;
        }
        std::array<u64, 4> ticks{};
        const vk::Result result =
            device.getQueryPoolResults(*query_pool, slot * 4, 4, sizeof(ticks), ticks.data(),
                                       sizeof(u64), vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess) {
            continue; // not finished yet; never waited for
        }
        timing_pending[slot] = false;
        const float to_ms = timestamp_period_ns * 1e-6f;
        const float prep = static_cast<float>(ticks[1] - ticks[0]) * to_ms;
        const float warp = static_cast<float>(ticks[3] - ticks[2]) * to_ms;
        ++timing_sum.frames;
        timing_sum.prep_ms += prep;
        timing_sum.warp_ms += warp;
        timing_sum.prep_max_ms = std::max(timing_sum.prep_max_ms, prep);
        timing_sum.warp_max_ms = std::max(timing_sum.warp_max_ms, warp);
    }
}

VrStereoWarp::GpuTimes VrStereoWarp::TakeGpuTimes() {
    GpuTimes result = std::exchange(timing_sum, GpuTimes{});
    if (result.frames != 0) {
        result.prep_ms /= static_cast<float>(result.frames);
        result.warp_ms /= static_cast<float>(result.frames);
    }
    return result;
}

void VrStereoWarp::BeginFrame(u64 frame_tag) {
    if (query_pool && timing_written != 0) {
        // The frame's spans are complete only when all four timestamps were written.
        timing_pending[timing_slot] = timing_written == 0xF;
        timing_slot = (timing_slot + 1) % TimingSlots;
        timing_written = 0;
    }
    if (query_pool) {
        CollectTimes();
    }
    if (frame_tag != 0) {
        NextSlot(); // a new capture starts
    }
    info.frame_tag = frame_tag;
    info.color_valid = false;
    info.depth_valid = false;
    info.hud_valid = false;
    info.mask_valid = false;
}

void VrStereoWarp::EnableLateRing(const std::atomic<u32>* busy, u32 count) {
    late_busy = busy;
    slot_count = std::clamp<u32>(count, 3, LateSlots);
}

void VrStereoWarp::NextSlot() {
    ++captures;
    if (slot_count <= 1) {
        return;
    }
    // The slot written longest ago that the presenter neither shows nor has queued. There is
    // always one: it holds at most count - 1 (the pair on display and the waiting ones).
    const u32 busy = late_busy != nullptr ? late_busy->load(std::memory_order_acquire) : 0u;
    s32 best = -1;
    for (u32 i = 0; i < slot_count; ++i) {
        if ((busy & (1u << i)) == 0 && (best < 0 || slot_written[i] < slot_written[best])) {
            best = static_cast<s32>(i);
        }
    }
    slot_index = best >= 0 ? static_cast<u32>(best) : (slot_index + 1) % slot_count;
    slot_written[slot_index] = captures;
}

bool VrStereoWarp::HasCapture() const {
    const auto& scene = Slot().scene;
    return info.color_valid && info.depth_valid && scene.image && disparity.image &&
           scene.extent == disparity.extent;
}

VrStereoWarp::SceneImage VrStereoWarp::Scene() const {
    const auto& scene = Slot().scene;
    if (!info.color_valid || !scene.image) {
        return {};
    }
    return {
        .image = scene.image,
        .raw = *scene.raw_view,
        .decoded = scene.srgb_view ? *scene.srgb_view : vk::ImageView{},
        .storage = scene.storage_view ? *scene.storage_view : vk::ImageView{},
        .extent = scene.extent,
        .format = scene.format,
    };
}

VrStereoWarp::LateCapture VrStereoWarp::GetLateCapture(const Settings& settings) const {
    const CaptureSlot& slot = Slot();
    if (!LateRingEnabled() || !HasCapture() || !slot.inverse_depth.image || !slot.tiles.image ||
        slot.inverse_depth.extent != slot.scene.extent) {
        return {};
    }
    LateCapture late{};
    late.valid = true;
    late.slot = slot_index;
    late.size = slot.scene.extent;
    const bool srgb = settings.source_encoding == SourceEncoding::Srgb;
    late.scene = srgb ? slot.scene.DecodedView() : *slot.scene.raw_view;
    late.scene_decode = srgb && !slot.scene.HwDecode();
    late.inverse_depth = *slot.inverse_depth.raw_view;
    late.tiles = *slot.tiles.raw_view;
    if (settings.hud_mode == HudMode::Diff && info.hud_valid && slot.hud.image &&
        slot.hud.extent == slot.scene.extent) {
        late.hud = srgb ? slot.hud.DecodedView() : *slot.hud.raw_view;
        late.hud_decode = srgb && !slot.hud.HwDecode();
    }
    // Our in-frame items' coverage: the HUD diff's exclusion (with a HUD only, FlagItemMask) and
    // late_warp_motion's body classification (they stay in tracking space).
    if (info.mask_valid && slot.item_mask.image && slot.item_mask.extent == slot.scene.extent) {
        late.item_mask = *slot.item_mask.raw_view;
    }
    late.camera = info.camera;
    late.disparity = info.disparity;
    late.reach_px = info.reach_px;
    late.settings = settings;
    return late;
}

void VrStereoWarp::RetireImage(OwnedImage& img) {
    if (img.image) {
        // Commands recorded up to the current tick may still use it.
        scheduler.DeferOperation([image = std::move(img.image), raw = std::move(img.raw_view),
                                  srgb = std::move(img.srgb_view),
                                  storage = std::move(img.storage_view)]() mutable {
            storage.reset();
            srgb.reset();
            raw.reset();
            image.Destroy();
        });
    }
    img.raw_view.reset();
    img.srgb_view.reset();
    img.storage_view.reset();
    img.extent = {};
    img.format = vk::Format::eUndefined;
}

void VrStereoWarp::EnsureImage(OwnedImage& img, vk::Extent2D extent, vk::Format format,
                               vk::ImageUsageFlags usage, bool srgb_views, const char* name,
                               const vk::ComponentMapping& components) {
    if (img.image && img.extent == extent && img.format == format && img.components == components) {
        return;
    }
    RetireImage(img);

    // raw_view is never a decoding view; the sRGB twin (if any) does the hardware decode.
    const vk::Format raw_format = srgb_views ? UnormTwin(format) : format;
    const vk::Format srgb_format = srgb_views ? SrgbTwin(raw_format) : vk::Format::eUndefined;
    const bool mutable_format =
        raw_format != format || (srgb_format != vk::Format::eUndefined && srgb_format != format);

    const vk::ImageCreateInfo image_ci{
        .flags = mutable_format ? vk::ImageCreateFlagBits::eMutableFormat : vk::ImageCreateFlags{},
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent{
            .width = extent.width,
            .height = extent.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.image = VideoCore::UniqueImage(device, allocator);
    img.image.Create(image_ci);
    SetObjectName(device, static_cast<vk::Image>(img.image), "VR DIBR {} {}x{}", name, extent.width,
                  extent.height);

    vk::ImageViewCreateInfo view_ci{
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = raw_format,
        .components = components,
        .subresourceRange = ColorRange,
    };
    // A colour copy with storage usage (the scene: the UI layer's veil): the swizzled and sRGB
    // views are sampled only, and an identity raw view is the storage view. (The disparity and tile
    // images use their raw views as storage.)
    const bool storage = static_cast<bool>(usage & vk::ImageUsageFlagBits::eStorage) && srgb_views;
    const vk::ImageViewUsageCreateInfo sampled_usage{.usage =
                                                         usage & ~vk::ImageUsageFlagBits::eStorage};
    if (storage) {
        view_ci.pNext = &sampled_usage;
    }
    img.raw_view = Check<"create vr dibr image view">(device.createImageViewUnique(view_ci));
    SetObjectName(device, *img.raw_view, "VR DIBR {} view", name);
    if (srgb_format != vk::Format::eUndefined) {
        view_ci.format = srgb_format;
        img.srgb_view = Check<"create vr dibr srgb view">(device.createImageViewUnique(view_ci));
        SetObjectName(device, *img.srgb_view, "VR DIBR {} srgb view", name);
    }
    if (storage) {
        const vk::ImageViewUsageCreateInfo storage_usage{.usage = vk::ImageUsageFlagBits::eStorage};
        img.storage_view = Check<"create vr dibr storage view">(device.createImageViewUnique({
            .pNext = &storage_usage,
            .image = img.image,
            .viewType = vk::ImageViewType::e2D,
            .format = raw_format,
            .subresourceRange = ColorRange,
        }));
        SetObjectName(device, *img.storage_view, "VR DIBR {} storage view", name);
    }
    img.extent = extent;
    img.format = format;
    img.components = components;
}

void VrStereoWarp::CopyInto(vk::CommandBuffer cmdbuf, OwnedImage& dst, vk::Image src,
                            vk::ImageLayout src_layout, vk::Extent2D extent,
                            vk::SampleCountFlagBits samples) const {
    // The previous frame's warp may still read dst (write-after-read: execution dependency).
    const vk::ImageMemoryBarrier2 pre{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = dst.image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre,
    });
    const vk::Extent3D extent3d{
        .width = extent.width,
        .height = extent.height,
        .depth = 1,
    };
    if (samples != vk::SampleCountFlagBits::e1) {
        const vk::ImageResolve region{
            .srcSubresource = ColorLayers,
            .srcOffset = {},
            .dstSubresource = ColorLayers,
            .dstOffset = {},
            .extent = extent3d,
        };
        cmdbuf.resolveImage(src, src_layout, dst.image, vk::ImageLayout::eTransferDstOptimal,
                            region);
    } else {
        const vk::ImageCopy region{
            .srcSubresource = ColorLayers,
            .srcOffset = {},
            .dstSubresource = ColorLayers,
            .dstOffset = {},
            .extent = extent3d,
        };
        cmdbuf.copyImage(src, src_layout, dst.image, vk::ImageLayout::eTransferDstOptimal, region);
    }
    const vk::ImageMemoryBarrier2 post{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = dst.image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post,
    });
}

bool VrStereoWarp::Capture(vk::CommandBuffer cmdbuf, const CaptureInputs& in,
                           const Settings& settings) {
    const bool depth_ok = CaptureDepth(cmdbuf, in.depth_view, in.depth_layout, in.depth_extent,
                                       in.color_extent, in.camera, settings);
    const bool color_ok = CaptureColor(cmdbuf, in.color_image, in.color_layout, in.color_format,
                                       in.color_extent, in.color_samples);
    return depth_ok && color_ok && HasCapture();
}

bool VrStereoWarp::CaptureDepth(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageLayout depth_layout, vk::Extent2D depth_extent,
                                vk::Extent2D scene_extent, const CameraParams& camera,
                                const Settings& settings, const StableDepthInput* stable) {
    info.depth_valid = false;
    if (!depth_view || depth_extent.width == 0 || depth_extent.height == 0 ||
        scene_extent.width == 0 || scene_extent.height == 0) {
        return false;
    }
    EnsureImage(disparity, scene_extent, vk::Format::eR32Sfloat,
                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage, false,
                "disparity");
    const vk::Extent2D tile_extent{
        .width = DivCeil(scene_extent.width, static_cast<u32>(VrStereo::kTileSize)),
        .height = DivCeil(scene_extent.height, static_cast<u32>(VrStereo::kTileSize)),
    };
    OwnedImage& tiles = Slot().tiles;
    EnsureImage(tiles, tile_extent, vk::Format::eR32G32B32A32Sfloat,
                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage, false,
                "tiles");
    // The late warp's true 1/z (written with the disparity; else the disparity is bound there).
    const bool write_w = LateRingEnabled();
    OwnedImage& inverse_depth = Slot().inverse_depth;
    if (write_w) {
        EnsureImage(inverse_depth, scene_extent, vk::Format::eR32Sfloat,
                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage, false,
                    "inverse depth");
    }

    const VrStereo::DisparityParams params = VrStereo::MakeDisparityParams(
        camera, static_cast<float>(scene_extent.width), settings.knee_m, settings.min_depth_m);
    const VrStereo::ProxyParams proxy =
        VrStereo::MakeProxyParams(settings.proxy, camera, static_cast<float>(scene_extent.width),
                                  static_cast<float>(scene_extent.height));

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/VR DIBR prep",
        });
    }
    Timestamp(cmdbuf, 0);

    // Overwrite the images completely, after the previous frame's warp (and the late warp, on the
    // frame thread's earlier submissions) has read them.
    std::array<vk::ImageMemoryBarrier2, 3> pre{};
    std::array<vk::ImageMemoryBarrier2, 3> post{};
    const u32 num_outputs = write_w ? 3u : 2u;
    const std::array<vk::Image, 3> outputs{disparity.image, tiles.image,
                                           write_w ? static_cast<vk::Image>(inverse_depth.image)
                                                   : vk::Image{}};
    for (size_t i = 0; i < num_outputs; ++i) {
        pre[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = outputs[i],
            .subresourceRange = ColorRange,
        };
        post[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = outputs[i],
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = num_outputs,
        .pImageMemoryBarriers = pre.data(),
    });

    const bool stable_on = stable != nullptr && stable->stable && stable->raw;
    const std::array<vk::DescriptorImageInfo, 6> image_infos{{
        {
            .imageView = depth_view,
            .imageLayout = depth_layout,
        },
        {
            .imageView = *disparity.raw_view,
            .imageLayout = vk::ImageLayout::eGeneral,
        },
        {
            .imageView = *tiles.raw_view,
            .imageLayout = vk::ImageLayout::eGeneral,
        },
        {
            .imageView = write_w ? *inverse_depth.raw_view : *disparity.raw_view,
            .imageLayout = vk::ImageLayout::eGeneral,
        },
        // aa_mode=taa's stable depth and the game's own depth (unused copies of depth_view
        // without it).
        {
            .imageView = stable_on ? stable->stable : depth_view,
            .imageLayout = stable_on ? vk::ImageLayout::eGeneral : depth_layout,
        },
        {
            .imageView = stable_on ? stable->raw : depth_view,
            .imageLayout = stable_on ? stable->raw_layout : depth_layout,
        },
    }};
    std::array<vk::WriteDescriptorSet, 6> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = (i == 0 || i >= 4) ? vk::DescriptorType::eSampledImage
                                                 : vk::DescriptorType::eStorageImage,
            .pImageInfo = &image_infos[i],
        };
    }
    const PrepPush push{
        .size = {static_cast<s32>(scene_extent.width), static_cast<s32>(scene_extent.height)},
        .depth_size = {static_cast<s32>(depth_extent.width), static_cast<s32>(depth_extent.height)},
        .disparity = params,
        .proxy = proxy,
        .write_w = write_w ? 1.0f : 0.0f,
        .stable_on = stable_on ? 1.0f : 0.0f,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *prep_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *prep_layout, 0, writes);
    ScratchPush::Invalidate();
    cmdbuf.pushConstants(*prep_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch(tile_extent.width, tile_extent.height, 1);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = num_outputs,
        .pImageMemoryBarriers = post.data(),
    });
    Timestamp(cmdbuf, 1);

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    info.camera = camera;
    info.disparity = params;
    info.proxy = proxy;
    info.reach_px = VrStereo::ReachPx(params);
    info.depth_valid = true;
    if (info.color_valid) {
        ++info.seq;
    }
    return true;
}

bool VrStereoWarp::CaptureColor(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout,
                                vk::Format format, vk::Extent2D extent,
                                vk::SampleCountFlagBits samples,
                                const vk::ComponentMapping& components) {
    info.color_valid = false;
    if (!src || format == vk::Format::eUndefined || extent.width == 0 || extent.height == 0) {
        return false;
    }
    // Storage too where the format allows it: the UI layer veils the scene in place.
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eSampled;
    if (instance.IsFormatSupported(UnormTwin(format), vk::FormatFeatureFlagBits2::eStorageImage)) {
        usage |= vk::ImageUsageFlagBits::eStorage;
    }
    OwnedImage& scene = Slot().scene;
    EnsureImage(scene, extent, format, usage, true, "scene", components);
    CopyInto(cmdbuf, scene, src, src_layout, extent, samples);
    info.size = extent;
    info.color_format = format;
    info.color_valid = true;
    if (info.depth_valid) {
        ++info.seq;
    }
    return true;
}

bool VrStereoWarp::CaptureItemMask(vk::CommandBuffer cmdbuf, vk::Image src,
                                   vk::Extent2D extent) {
    info.mask_valid = false;
    OwnedImage& item_mask = Slot().item_mask;
    if (!src || !info.color_valid || extent != Slot().scene.extent) {
        return false;
    }
    EnsureImage(item_mask, extent, vk::Format::eR8Unorm,
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled, false,
                "item mask");
    CopyInto(cmdbuf, item_mask, src, vk::ImageLayout::eTransferSrcOptimal, extent,
             vk::SampleCountFlagBits::e1);
    info.mask_valid = true;
    return true;
}

bool VrStereoWarp::CaptureHud(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout,
                              vk::Format format, vk::Extent2D extent,
                              vk::SampleCountFlagBits samples,
                              const vk::ComponentMapping& components) {
    info.hud_valid = false;
    if (!src || format == vk::Format::eUndefined || extent.width == 0 || extent.height == 0) {
        return false;
    }
    if ((timing_written & 0x3) == 0x3) {
        Timestamp(cmdbuf, 2); // the warp span starts with the HUD copy
    }
    OwnedImage& hud = Slot().hud;
    EnsureImage(hud, extent, format,
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                    vk::ImageUsageFlagBits::eSampled,
                true, "hud", components);
    CopyInto(cmdbuf, hud, src, src_layout, extent, samples);
    info.hud_valid = true;
    return true;
}

bool VrStereoWarp::Warp(vk::CommandBuffer cmdbuf, const std::array<const EyeTarget*, 2>& eyes,
                        const Settings& settings, const ExternalHud& external_hud) {
    if (!HasCapture()) {
        return false;
    }
    const OwnedImage& scene = Slot().scene;
    const OwnedImage& hud = Slot().hud;
    const OwnedImage& tiles = Slot().tiles;
    const OwnedImage& item_mask = Slot().item_mask;
    const vk::Extent2D src_size = scene.extent;

    // Clip each eye's rect so every store lands inside its target.
    std::array<vk::Rect2D, 2> rects{};
    std::array<bool, 2> active{};
    for (u32 e = 0; e < 2; ++e) {
        const EyeTarget* t = eyes[e];
        if (!t || !t->image || !t->storage_view || t->dst_offset.x < 0 || t->dst_offset.y < 0) {
            continue;
        }
        const s64 room_w = static_cast<s64>(t->target_extent.width) - t->dst_offset.x;
        const s64 room_h = static_cast<s64>(t->target_extent.height) - t->dst_offset.y;
        const u32 w = static_cast<u32>(std::clamp<s64>(t->src_rect.extent.width, 0, room_w));
        const u32 h = static_cast<u32>(std::clamp<s64>(t->src_rect.extent.height, 0, room_h));
        if (w == 0 || h == 0) {
            continue;
        }
        rects[e] = {.offset = t->src_rect.offset, .extent = {.width = w, .height = h}};
        active[e] = true;
    }
    if (!active[0] && !active[1]) {
        return false;
    }
    const u32 first_eye = active[0] ? 0 : 1;
    const u32 num_eyes = (active[0] && active[1]) ? 2 : 1;
    const EyeTarget& t0 = *eyes[first_eye];
    const EyeTarget& t1 = num_eyes == 2 ? *eyes[1] : t0;

    // ---- sources and flags ----
    const bool srgb_source = settings.source_encoding == SourceEncoding::Srgb;
    u32 flags = 0;
    if (settings.catmull_rom) {
        flags |= FlagCatmullRom;
    }
    if (settings.debug_holes) {
        flags |= FlagDebugHoles;
    }
    if (settings.debug_hud) {
        flags |= FlagDebugHud;
    }
    const vk::ImageView scene_view = srgb_source ? scene.DecodedView() : *scene.raw_view;
    if (srgb_source && !scene.HwDecode()) {
        flags |= FlagDecodeScene;
    }
    vk::ImageView hud_view{};
    const bool own_hud = info.hud_valid && hud.image && hud.extent == scene.extent;
    if (settings.hud_mode == HudMode::Diff) {
        if (external_hud.view) {
            hud_view = external_hud.view;
            flags |= FlagHudDiff | (srgb_source && external_hud.needs_decode ? FlagDecodeHud : 0);
        } else if (own_hud) {
            hud_view = srgb_source ? hud.DecodedView() : *hud.raw_view;
            flags |= FlagHudDiff | (srgb_source && !hud.HwDecode() ? FlagDecodeHud : 0);
        }
    } else if (settings.hud_mode == HudMode::Premultiplied) {
        if (external_hud.view) {
            hud_view = external_hud.view; // must be a raw (non-decoding) view
            flags |= FlagHudPremul;
        } else if (own_hud) {
            hud_view = *hud.raw_view;
            flags |= FlagHudPremul;
        }
    }
    if (!hud_view) {
        hud_view = scene_view; // unused by the shader, but the binding must be valid
    }
    const bool mask = (flags & FlagHudDiff) != 0 && info.mask_valid && item_mask.image &&
                      item_mask.extent == scene.extent;
    if (mask) {
        flags |= FlagItemMask;
    }
    const vk::ImageView mask_view = mask ? *item_mask.raw_view : scene_view;

    // ---- constants ----
    WarpPush push{};
    const float hud_depth_units = settings.hud_depth_m * info.camera.units_per_metre;
    for (u32 e = 0; e < 2; ++e) {
        if (!active[e]) {
            continue;
        }
        const EyeTarget& t = *eyes[e];
        push.crop[e][0] = rects[e].offset.x;
        push.crop[e][1] = rects[e].offset.y;
        push.crop[e][2] = static_cast<s32>(rects[e].extent.width);
        push.crop[e][3] = static_cast<s32>(rects[e].extent.height);
        push.dst[e][0] = t.dst_offset.x;
        push.dst[e][1] = t.dst_offset.y;
        push.dst[e][2] =
            VrStereo::HudShiftPx(info.disparity.k_px, hud_depth_units, static_cast<int>(e));
        if (t.bgra) {
            flags |= e == 0 ? FlagBgra0 : FlagBgra1;
        }
    }
    push.dst[0][3] = static_cast<s32>(settings.color.Packed());
    push.src_size[0] = static_cast<s32>(src_size.width);
    push.src_size[1] = static_cast<s32>(src_size.height);
    push.tau = settings.tau_px;
    push.reach = info.reach_px;
    push.hud_thresh = settings.hud_threshold;
    push.gamma = settings.gamma > 0.0f ? settings.gamma : 1.0f;
    // Depth: zn = (1/n - 1/z) / (1/n - 1/f) with 1/z = D_lin * units_per_metre / K (metres).
    const vk::DeviceSize eye0_floats =
        active[0] ? vk::DeviceSize{rects[0].extent.width} * rects[0].extent.height : 0;
    const vk::DeviceSize eye1_floats =
        active[1] ? vk::DeviceSize{rects[1].extent.width} * rects[1].extent.height : 0;
    EnsureDepthBuffer((eye0_floats + eye1_floats) * sizeof(float));
    depth_offset = {0, eye0_floats * sizeof(float)};
    if (settings.depth) {
        const float span = 1.0f / DepthNearM - 1.0f / DepthFarM;
        const auto& d = info.disparity;
        flags |= FlagDepth;
        push.knee_px = d.knee_px;
        push.knee_r = d.floor_px > d.knee_px ? d.floor_px - d.knee_px : 0.0f;
        push.z_a = (1.0f / DepthNearM) / span;
        push.z_b = d.k_px > 0.0f ? info.camera.units_per_metre / (d.k_px * span) : 0.0f;
        push.z_hud = settings.hud_depth_m > 0.0f
                         ? (1.0f / DepthNearM - 1.0f / settings.hud_depth_m) / span
                         : 1.0f;
        if (settings.depth_debug_m > 0.0f) {
            push.z_b = 0.0f;
            push.z_a = (1.0f / DepthNearM - 1.0f / settings.depth_debug_m) / span;
            push.z_hud = push.z_a;
        }
    }
    push.depth_base1 = static_cast<u32>(eye0_floats);
    push.flags = flags;
    push.first_eye = first_eye;

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/VR DIBR warp",
        });
    }
    if ((timing_written & 0x3) == 0x3 && (timing_written & 0x4) == 0) {
        Timestamp(cmdbuf, 2); // no HUD copy this frame
    }

    // ---- target layouts: -> General (one barrier per distinct image) ----
    const bool shared_target = num_eyes == 2 && t0.image == t1.image;
    const u32 num_targets = (num_eyes == 2 && !shared_target) ? 2 : 1;
    const std::array<const EyeTarget*, 2> targets{&t0, &t1};
    std::array<vk::ImageMemoryBarrier2, 2> pre{};
    std::array<vk::ImageMemoryBarrier2, 2> post{};
    for (u32 i = 0; i < num_targets; ++i) {
        const EyeTarget& t = *targets[i];
        const auto [dst_stage, dst_access] = ConsumerOf(t.new_layout);
        pre[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = t.old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = t.image,
            .subresourceRange = ColorRange,
        };
        post[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = t.new_layout,
            .image = t.image,
            .subresourceRange = ColorRange,
        };
    }
    // The depth buffer: the previous frame's copy out of it is done before it is overwritten.
    const vk::BufferMemoryBarrier2 depth_pre{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .buffer = depth_buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &depth_pre,
        .imageMemoryBarrierCount = num_targets,
        .pImageMemoryBarriers = pre.data(),
    });

    // ---- dispatch ----
    const std::array<vk::DescriptorImageInfo, 6> image_infos{{
        {.imageView = *disparity.raw_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView = *tiles.raw_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView = scene_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView = hud_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView = (active[0] ? *eyes[0] : t0).storage_view,
         .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = (active[1] ? *eyes[1] : t0).storage_view,
         .imageLayout = vk::ImageLayout::eGeneral},
    }};
    std::array<vk::WriteDescriptorSet, 8> writes{};
    for (u32 i = 0; i < image_infos.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType =
                i < 4 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage,
            .pImageInfo = &image_infos[i],
        };
    }
    const vk::DescriptorBufferInfo depth_info{
        .buffer = depth_buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    writes[6] = {
        .dstBinding = 6,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer,
        .pBufferInfo = &depth_info,
    };
    const vk::DescriptorImageInfo mask_info{
        .imageView = mask_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    writes[7] = {
        .dstBinding = 7,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &mask_info,
    };
    u32 max_w = 0;
    u32 max_h = 0;
    for (u32 e = 0; e < 2; ++e) {
        if (active[e]) {
            max_w = std::max(max_w, rects[e].extent.width);
            max_h = std::max(max_h, rects[e].extent.height);
        }
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        settings.color.Active() ? *warp_pipeline : *warp_neutral_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *warp_layout, 0, writes);
    ScratchPush::Invalidate();
    cmdbuf.pushConstants(*warp_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch(DivCeil(max_w, static_cast<u32>(VrStereo::kWarpGroupWidth)),
                    DivCeil(max_h, static_cast<u32>(VrStereo::kWarpGroupRows)), num_eyes);

    const vk::BufferMemoryBarrier2 depth_post{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = depth_buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &depth_post,
        .imageMemoryBarrierCount = num_targets,
        .pImageMemoryBarriers = post.data(),
    });
    if ((timing_written & 0x7) == 0x7) {
        Timestamp(cmdbuf, 3);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return true;
}

bool VrStereoWarp::Warp(vk::CommandBuffer cmdbuf, const std::array<const EyeTarget*, 2>& eyes,
                        const Settings& settings) {
    return Warp(cmdbuf, eyes, settings, ExternalHud{});
}

void VrStereoWarp::EnsureEyeImages(EyeImages& images, vk::Extent2D left, vk::Extent2D right) {
    const std::array<vk::Extent2D, 2> extents{left, right};
    for (u32 e = 0; e < 2; ++e) {
        if (images.image[e] && images.extent[e] == extents[e]) {
            continue;
        }
        if (images.image[e]) {
            // Only resize a frame the present thread is done with (after its fence wait).
            scheduler.DeferOperation(
                [image = std::move(images.image[e]), view = std::move(images.view[e])]() mutable {
                    view.reset();
                    image.Destroy();
                });
        }
        images.view[e].reset();
        images.extent[e] = extents[e];
        images.layout[e] = vk::ImageLayout::eUndefined;
        if (extents[e].width == 0 || extents[e].height == 0) {
            continue;
        }
        const vk::ImageCreateInfo image_ci{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent{
                .width = extents[e].width,
                .height = extents[e].height,
                .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        images.image[e] = VideoCore::UniqueImage(device, allocator);
        images.image[e].Create(image_ci);
        SetObjectName(device, static_cast<vk::Image>(images.image[e]), "VR DIBR eye {} {}x{}", e,
                      extents[e].width, extents[e].height);
        images.view[e] = Check<"create vr dibr eye view">(device.createImageViewUnique({
            .image = images.image[e],
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = ColorRange,
        }));
    }
}

bool VrStereoWarp::WarpToEyeImages(vk::CommandBuffer cmdbuf, EyeImages& images,
                                   const std::array<vk::Rect2D, 2>& src_rects,
                                   const Settings& settings, vk::ImageLayout final_layout,
                                   bool bgra, const ExternalHud& external_hud) {
    if (!HasCapture()) {
        return false;
    }
    EnsureEyeImages(images, src_rects[0].extent, src_rects[1].extent);
    std::array<EyeTarget, 2> targets{};
    std::array<const EyeTarget*, 2> ptrs{};
    for (u32 e = 0; e < 2; ++e) {
        if (!images.image[e]) {
            continue;
        }
        targets[e] = EyeTarget{
            .image = images.image[e],
            .storage_view = *images.view[e],
            .target_extent = images.extent[e],
            .src_rect = src_rects[e],
            .dst_offset = {},
            .old_layout = images.layout[e],
            .new_layout = final_layout,
            .bgra = bgra,
        };
        ptrs[e] = &targets[e];
    }
    if (!Warp(cmdbuf, ptrs, settings, external_hud)) {
        return false;
    }
    for (u32 e = 0; e < 2; ++e) {
        if (ptrs[e]) {
            images.layout[e] = final_layout;
        }
    }
    return true;
}

bool VrStereoWarp::WarpToEyeImages(vk::CommandBuffer cmdbuf, EyeImages& images,
                                   const std::array<vk::Rect2D, 2>& src_rects,
                                   const Settings& settings) {
    return WarpToEyeImages(cmdbuf, images, src_rects, settings,
                           vk::ImageLayout::eTransferSrcOptimal, true, ExternalHud{});
}

} // namespace Vulkan
