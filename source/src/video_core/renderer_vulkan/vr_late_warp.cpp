#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include "video_core/renderer_vulkan/vr_late_warp.h"

#ifdef ENABLE_OPENXR

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <vk_mem_alloc.h>
#include <openxr/openxr_platform.h>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_dibr_warp_comp.h"
#include "video_core/host_shaders/vr_eye_taa_comp.h"
#include "video_core/host_shaders/vr_late_classify_comp.h"
#include "video_core/host_shaders/vr_late_reproject_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vr_eye_taa_math.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"
#include "video_core/renderer_vulkan/vr_device_lost.h"

namespace Vulkan {

namespace {

constexpr u64 WaitImageTimeoutNs = 1'000'000; // 1 ms: never stall the frame thread
constexpr u64 ShutdownTimeoutNs = 2'000'000'000;

/// Mirrors LateConstants in vr_late_reproject.comp (std140 uniform buffer).
struct ReprojectConstants {
    float rsv[3][4];
    float c[4];
    float intr[4];
    s32 region[4];
    s32 size[4];
    float disp[4];
    float misc[4];
    float rwv[3][4]; // late_warp_motion: V -> S moved with the world
    float cw[4];     // xyz: V's centre in that frame; w: bits 1 motion, 2 debug body, 4 body mask
};
static_assert(sizeof(ReprojectConstants) == 208); // 13 vec4s, std140

/// Mirrors ClassifyConstants in vr_late_classify.comp (std140 uniform buffer).
struct ClassifyConstants {
    float intr[4];
    s32 size[4];  // width, height, capsule count
    float reach[4];
    float floor_plane[4];
    float caps[2 * VrLate::kMaxBodyCapsules][4];
};
static_assert(sizeof(ClassifyConstants) == 64 + 32 * VrLate::kMaxBodyCapsules);

/// Mirrors TaaPush in vr_eye_taa.comp (push constants).
struct TaaPush {
    float m[3][4];     // rows of M (the previous eye's axes <- this eye's); m[0][3]: this refresh's
                       // weight, m[1][3]: metres per game unit, m[2][3]: eye_taa_gate_px
    float t[4];        // this eye's position in the previous eye's frame (metres); eye_taa_edge_cap
    float intr[4];     // this eye: fx, fy, cx, cy (eye-image pixels)
    float intr_p[4];   // the previous eye's
    s32 size[4];       // this eye's w, h; the previous eye's
    s32 misc[4];       // column in eye_w; place in `eyes` (x, y); flags
};
static_assert(sizeof(TaaPush) == 128);
constexpr s32 TaaHistory = 1;
constexpr s32 TaaWrite8 = 2;
constexpr s32 TaaDebug = 4;
constexpr s32 TaaMoving = 8; // eye_taa_follow: the world moved since the history

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

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

} // namespace

VrLateWarp::VrLateWarp() = default;

VrLateWarp::~VrLateWarp() {
    Shutdown();
}

bool VrLateWarp::Fail(std::string_view what) {
    LOG_ERROR(Render_Vulkan, "VR late warp disabled: {}", what);
    failed = true;
    return false;
}

void VrLateWarp::Disable(std::string_view reason) {
    if (!failed) {
        LOG_WARNING(Render_Vulkan, "VR late warp disabled: {}", reason);
    }
    failed = true;
}

bool VrLateWarp::Init(const DeviceContext& device, XrSession xr_session, s64 format) {
    Shutdown();
    dev = device;
    session = xr_session;
    xr_format = format;
    vk_format = static_cast<vk::Format>(format);
    failed = false;
    if (vk_format != vk::Format::eB8G8R8A8Srgb && vk_format != vk::Format::eR8G8B8A8Srgb) {
        return Fail(fmt::format("the eye swapchains are {}, not an 8-bit sRGB format",
                                vk::to_string(vk_format)));
    }
    bgra = vk_format == vk::Format::eB8G8R8A8Srgb;
    if (!dev.device || !dev.queue || !dev.allocator || !dev.queue_mutex ||
        session == XR_NULL_HANDLE) {
        return Fail("missing device or session handles");
    }
    if (!CreatePipelines() || !CreateFrameResources()) {
        Shutdown();
        failed = true;
        return false;
    }
    // vr.ini eye_upscale / eye_sharpen: without it the eyes go at the render's pixels (logged).
    upscale.Init(dev.device, dev.allocator);
    initialized = true;
    return true;
}

bool VrLateWarp::CreatePipelines() {
    const auto make_layout = [&](std::span<const vk::DescriptorType> types, u32 push_size,
                                 vk::DescriptorSetLayout& set_layout,
                                 vk::PipelineLayout& layout) {
        std::array<vk::DescriptorSetLayoutBinding, 11> bindings{};
        for (u32 i = 0; i < types.size(); ++i) {
            bindings[i] = {
                .binding = i,
                .descriptorType = types[i],
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            };
        }
        auto [r1, sl] = dev.device.createDescriptorSetLayout({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(types.size()),
            .pBindings = bindings.data(),
        });
        if (r1 != vk::Result::eSuccess) {
            return false;
        }
        set_layout = sl;
        const vk::PushConstantRange push{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = push_size,
        };
        auto [r2, pl] = dev.device.createPipelineLayout({
            .setLayoutCount = 1,
            .pSetLayouts = &set_layout,
            .pushConstantRangeCount = push_size != 0 ? 1u : 0u,
            .pPushConstantRanges = push_size != 0 ? &push : nullptr,
        });
        if (r2 != vk::Result::eSuccess) {
            return false;
        }
        layout = pl;
        return true;
    };
    const auto make_pipeline = [&](std::string_view code, std::vector<std::string> defines,
                                   vk::PipelineLayout layout, const char* name,
                                   vk::Pipeline& pipeline) {
        const vk::ShaderModule module =
            Compile(code, vk::ShaderStageFlagBits::eCompute, dev.device, std::move(defines));
        if (!module) {
            return false;
        }
        const vk::ComputePipelineCreateInfo info{
            .stage{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = layout,
        };
        auto [result, p] = dev.device.createComputePipeline({}, info);
        dev.device.destroyShaderModule(module);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        pipeline = p;
        SetObjectName(dev.device, pipeline, std::string_view{name});
        return true;
    };
    using T = vk::DescriptorType;
    // Pass 1: S's 1/z and its tiles (sampled) -> V's map, disparity, tiles (storage); constants;
    // late_warp_motion's signed 1/z and layer tiles (sampled).
    constexpr std::array<T, 8> reproject_types{T::eSampledImage, T::eSampledImage,
                                               T::eStorageImage, T::eStorageImage,
                                               T::eStorageImage, T::eUniformBuffer,
                                               T::eSampledImage, T::eSampledImage};
    if (!make_layout(reproject_types, 0, reproject_set_layout, reproject_layout) ||
        !make_pipeline(HostShaders::VR_LATE_REPROJECT_COMP, {}, reproject_layout,
                       "vr_late_reproject.comp", reproject_pipeline)) {
        return Fail("the reprojection pipeline");
    }
    // Pass 2: vr_dibr_warp.comp's bindings (V's disparity and tiles, S's scene and HUD, both eyes,
    // the depth buffer, S's item mask) and LATE_WARP's (V's map, S's 1/z, the eyes' 1/z).
    constexpr std::array<T, 11> warp_types{T::eSampledImage, T::eSampledImage, T::eSampledImage,
                                           T::eSampledImage, T::eStorageImage, T::eStorageImage,
                                           T::eStorageBuffer, T::eSampledImage, T::eSampledImage,
                                           T::eSampledImage, T::eStorageImage};
    if (!make_layout(warp_types, sizeof(VrStereo::WarpPush), warp_set_layout, warp_layout) ||
        !make_pipeline(HostShaders::VR_DIBR_WARP_COMP, {"LATE_WARP"}, warp_layout,
                       "vr_dibr_warp.comp (LATE_WARP)", warp_pipeline)) {
        return Fail("the eye warp pipeline");
    }
    // late_warp_fetch_once: the same pass with each 1/z and colour of the Catmull-Rom kernel
    // fetched once (optional: without it the plain one runs).
    if (!make_pipeline(HostShaders::VR_DIBR_WARP_COMP, {"LATE_WARP", "WARP_FETCH_ONCE"},
                       warp_layout, "vr_dibr_warp.comp (LATE_WARP, fetch once)",
                       warp_pipeline_once)) {
        warp_pipeline_once = vk::Pipeline{};
        LOG_WARNING(Render_Vulkan, "VR late warp: no fetch-once warp pipeline (the plain one runs)");
    }
    // late_warp_motion, once per capture: S's 1/z (sampled) -> signed 1/z, layer tiles (storage);
    // constants; S's item mask (sampled).
    constexpr std::array<T, 5> classify_types{T::eSampledImage, T::eStorageImage,
                                              T::eStorageImage, T::eUniformBuffer,
                                              T::eSampledImage};
    if (!make_layout(classify_types, 0, classify_set_layout, classify_layout) ||
        !make_pipeline(HostShaders::VR_LATE_CLASSIFY_COMP, {}, classify_layout,
                       "vr_late_classify.comp", classify_pipeline)) {
        return Fail("the body classification pipeline");
    }
    // eye_taa: this eye, its history, the eyes' 1/z (sampled) -> the new history, `eyes` (storage).
    // Optional: without it eye_taa stays off (logged).
    constexpr std::array<T, 5> taa_types{T::eSampledImage, T::eSampledImage, T::eSampledImage,
                                         T::eStorageImage, T::eStorageImage};
    if (!make_layout(taa_types, sizeof(TaaPush), taa_set_layout, taa_layout) ||
        !make_pipeline(HostShaders::VR_EYE_TAA_COMP, {}, taa_layout, "vr_eye_taa.comp",
                       taa_pipeline)) {
        LOG_WARNING(Render_Vulkan, "VR late warp: no eye temporal AA pipeline (eye_taa off)");
        taa_pipeline = vk::Pipeline{};
    }
    return true;
}

bool VrLateWarp::CreateFrameResources() {
    auto [pool_result, pool] = dev.device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                 vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = dev.queue_family,
    });
    if (pool_result != vk::Result::eSuccess) {
        return Fail("command pool");
    }
    command_pool = pool;
    auto [cmd_result, cmds] = dev.device.allocateCommandBuffers({
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 2,
    });
    if (cmd_result != vk::Result::eSuccess || cmds.size() != 2) {
        return Fail("command buffers");
    }
    for (u32 i = 0; i < 2; ++i) {
        slots[i].cmd = cmds[i];
        auto [fence_result, fence] = dev.device.createFence({});
        if (fence_result != vk::Result::eSuccess) {
            return Fail("fence");
        }
        slots[i].fence = fence;
    }
    const auto limits = dev.physical_device.getProperties().limits;
    const vk::DeviceSize align =
        std::max<vk::DeviceSize>(limits.minUniformBufferOffsetAlignment, 16);
    classify_offset = (sizeof(ReprojectConstants) + align - 1) / align * align;
    // vignette_in_warp: the frame's vignettes (VrVignetteWarp::Block) third.
    vignette_offset = (classify_offset + sizeof(ClassifyConstants) + align - 1) / align * align;
    uniform_stride =
        (vignette_offset + sizeof(VrVignetteWarp::Block) + align - 1) / align * align;
    const auto make_buffer = [&](Buffer& out, vk::DeviceSize size, vk::BufferUsageFlags usage,
                                 bool host) {
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = static_cast<VkBufferUsageFlags>(usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo alloc{};
        if (host) {
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
        } else {
            alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }
        VkBuffer buffer{};
        VmaAllocationInfo alloc_info{};
        if (vmaCreateBuffer(dev.allocator, &info, &alloc, &buffer, &out.allocation,
                            &alloc_info) != VK_SUCCESS) {
            return false;
        }
        out.buffer = vk::Buffer{buffer};
        out.mapped = alloc_info.pMappedData;
        return !host || out.mapped != nullptr;
    };
    if (!make_buffer(uniforms, 2 * uniform_stride, vk::BufferUsageFlagBits::eUniformBuffer, true) ||
        !make_buffer(dummy, 256, vk::BufferUsageFlagBits::eStorageBuffer, false)) {
        return Fail("buffers");
    }
    const auto families = dev.physical_device.getQueueFamilyProperties();
    if (dev.queue_family < families.size() && families[dev.queue_family].timestampValidBits > 0 &&
        limits.timestampPeriod > 0.0f) {
        auto [qp_result, qp] = dev.device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = 2 * QueriesPerSlot, // per slot: start, classified, reprojected,
                                              // warped, accumulated (eye_taa), upscaled, end
        });
        if (qp_result == vk::Result::eSuccess) {
            query_pool = qp;
            timestamp_period_ns = limits.timestampPeriod;
            timestamp_mask = families[dev.queue_family].timestampValidBits >= 64
                                 ? ~u64{0}
                                 : (u64{1} << families[dev.queue_family].timestampValidBits) - 1;
        }
    }
    return true;
}

bool VrLateWarp::EnsureImage(Image& image, vk::Extent2D extent, vk::Format format,
                             vk::ImageUsageFlags usage, std::string_view name) {
    if (image.image && image.extent == extent && image.format == format) {
        return true;
    }
    WaitIdle(); // our last frames may still use it
    DestroyImage(image);
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = static_cast<VkImageUsageFlags>(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(dev.allocator, &info, &alloc, &handle, &image.allocation, nullptr) !=
        VK_SUCCESS) {
        return Fail(fmt::format("{}: allocation failed", name));
    }
    image.image = vk::Image{handle};
    VmaAllocationInfo alloc_info{};
    vmaGetAllocationInfo(dev.allocator, image.allocation, &alloc_info);
    memory_bytes += alloc_info.size;
    auto [view_result, view] = dev.device.createImageView({
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    });
    if (view_result != vk::Result::eSuccess) {
        return Fail(fmt::format("{}: view failed", name));
    }
    image.view = view;
    image.extent = extent;
    image.format = format;
    q_map_cleared = false; // the next frame clears V's images (all three) once
    SetObjectName(dev.device, image.image, "VR late warp {} {}x{}", name, extent.width,
                  extent.height);
    return true;
}

void VrLateWarp::DestroyImage(Image& image) {
    if (image.view) {
        dev.device.destroyImageView(image.view);
    }
    if (image.image) {
        VmaAllocationInfo alloc_info{};
        vmaGetAllocationInfo(dev.allocator, image.allocation, &alloc_info);
        memory_bytes -= std::min<u64>(memory_bytes, alloc_info.size);
        vmaDestroyImage(dev.allocator, static_cast<VkImage>(image.image), image.allocation);
    }
    image = {};
}

// Queue lock held. A swapchain of at least `extent` (both eyes side by side).
bool VrLateWarp::EnsureSwapchain(vk::Extent2D extent) {
    if (swapchain != XR_NULL_HANDLE && swapchain_extent.width >= extent.width &&
        swapchain_extent.height >= extent.height) {
        return true;
    }
    DestroySwapchain();
    const vk::Extent2D alloc{(extent.width + 63) & ~63u, (extent.height + 63) & ~63u};
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = xr_format;
    info.sampleCount = 1;
    info.width = alloc.width;
    info.height = alloc.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (const XrResult r = xrCreateSwapchain(session, &info, &swapchain); XR_FAILED(r)) {
        swapchain = XR_NULL_HANDLE;
        return Fail(fmt::format("xrCreateSwapchain {}x{} failed ({})", alloc.width, alloc.height,
                                static_cast<s32>(r)));
    }
    u32 count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    std::vector<XrSwapchainImageVulkanKHR> images(count,
                                                  {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, {}});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    for (const auto& image : images) {
        swapchain_images.push_back(vk::Image{image.image});
    }
    swapchain_extent = alloc;
    LOG_INFO(Render_Vulkan, "VR late warp: swapchain {}x{} ({} images, both eyes {}x{})",
             alloc.width, alloc.height, count, extent.width, extent.height);
    return AcquireImage();
}

void VrLateWarp::DestroySwapchain() {
    if (swapchain == XR_NULL_HANDLE) {
        return;
    }
    WaitIdle();
    xrDestroySwapchain(swapchain);
    swapchain = XR_NULL_HANDLE;
    swapchain_images.clear();
    swapchain_extent = {};
    acquired = waited = false;
    has_image = false;
}

// Queue lock held.
bool VrLateWarp::AcquireImage() {
    const XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    u32 index = 0;
    if (const XrResult r = xrAcquireSwapchainImage(swapchain, &info, &index); XR_FAILED(r)) {
        Disable(fmt::format("xrAcquireSwapchainImage failed ({})", static_cast<s32>(r)));
        return false;
    }
    if (index >= swapchain_images.size()) {
        Disable("swapchain image index out of range");
        return false;
    }
    image_index = index;
    acquired = true;
    waited = false;
    if (queue_hooks.after_acquire) {
        queue_hooks.after_acquire(); // the runtime's own queue: its timeline after the acquire
    }
    return true;
}

void VrLateWarp::WaitIdle() {
    std::vector<vk::Fence> busy;
    for (Slot& s : slots) {
        if (s.fence && s.pending) {
            busy.push_back(s.fence);
        }
    }
    if (!busy.empty()) {
        if (dev.device.waitForFences(busy, VK_TRUE, ShutdownTimeoutNs) != vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "VR late warp: GPU work did not finish");
        }
        for (u32 i = 0; i < slots.size(); ++i) {
            if (slots[i].pending) {
                ReadTimestamps(i);
                slots[i].pending = false;
                (void)dev.device.resetFences(slots[i].fence);
            }
        }
    }
}

void VrLateWarp::Shutdown() {
    if (dev.device) {
        WaitIdle();
        DestroyImage(q_map);
        DestroyImage(v_disp);
        DestroyImage(v_tiles);
        DestroyImage(eyes);
        DestroyImage(eye_src[0]);
        DestroyImage(eye_src[1]);
        for (auto& pair : taa_hist) {
            for (Image& image : pair) {
                DestroyImage(image);
            }
        }
        DestroyImage(eye_w);
        upscale.Shutdown();
        DestroyImage(signed_w);
        DestroyImage(layer_tiles);
        for (Buffer* b : {&uniforms, &dummy}) {
            if (b->buffer) {
                vmaDestroyBuffer(dev.allocator, static_cast<VkBuffer>(b->buffer), b->allocation);
            }
            *b = {};
        }
        for (vk::Pipeline p : {reproject_pipeline, warp_pipeline, warp_pipeline_once,
                               classify_pipeline, taa_pipeline}) {
            if (p) {
                dev.device.destroyPipeline(p);
            }
        }
        for (vk::PipelineLayout l : {reproject_layout, warp_layout, classify_layout, taa_layout}) {
            if (l) {
                dev.device.destroyPipelineLayout(l);
            }
        }
        for (vk::DescriptorSetLayout l :
             {reproject_set_layout, warp_set_layout, classify_set_layout, taa_set_layout}) {
            if (l) {
                dev.device.destroyDescriptorSetLayout(l);
            }
        }
        if (query_pool) {
            dev.device.destroyQueryPool(query_pool);
        }
        for (Slot& s : slots) {
            if (s.fence) {
                dev.device.destroyFence(s.fence);
            }
        }
        if (command_pool) {
            dev.device.destroyCommandPool(command_pool);
        }
    }
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
    }
    swapchain = XR_NULL_HANDLE;
    swapchain_images.clear();
    swapchain_extent = {};
    reproject_pipeline = warp_pipeline = classify_pipeline = taa_pipeline = vk::Pipeline{};
    warp_pipeline_once = vk::Pipeline{};
    reproject_layout = warp_layout = classify_layout = taa_layout = vk::PipelineLayout{};
    reproject_set_layout = warp_set_layout = classify_set_layout = taa_set_layout =
        vk::DescriptorSetLayout{};
    taa_valid = taa_hist_ready = false;
    taa_parity = 0;
    layers_ready = false;
    classified_id = 0;
    query_pool = vk::QueryPool{};
    command_pool = vk::CommandPool{};
    slots = {};
    slot_index = 0;
    acquired = waited = has_image = false;
    initialized = false;
    memory_bytes = 0;
}

void VrLateWarp::ReadTimestamps(u32 slot) {
    if (!query_pool) {
        return;
    }
    std::array<u64, QueriesPerSlot> t{};
    if (dev.device.getQueryPoolResults(query_pool, slot * QueriesPerSlot, QueriesPerSlot,
                                       sizeof(t), t.data(), sizeof(u64),
                                       vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
        const auto ms = [&](u32 a, u32 b) {
            return static_cast<double>((t[b] - t[a]) & timestamp_mask) * timestamp_period_ns *
                   1e-6;
        };
        const double total = ms(0, 6);
        stats.gpu_ms += total;
        stats.gpu_max_ms = std::max(stats.gpu_max_ms, total);
        ++stats.gpu_samples;
        stats.reproject_ms += ms(1, 2);
        stats.warp_ms += ms(2, 3);
        stats.copy_ms += ms(5, 6);
        if (slot_classified[slot]) {
            stats.classify_ms += ms(0, 1);
            ++stats.classify_samples;
        }
        if (slot_taa[slot]) {
            stats.taa_ms += ms(3, 4);
            ++stats.taa_samples;
        }
        if (slot_upscaled[slot]) {
            stats.upscale_ms += ms(4, 5);
            ++stats.upscale_samples;
        }
        // FAST_GPU_PASS_PROFILE (bit 38): the late warp's share in the `FAST gpu passes:` line
        // (its own submissions: not in the draw stream's timestamps).
        using External = GpuPassProfiler::External;
        GpuPassProfiler::NoteExternal(External::Classify, ms(0, 1));
        GpuPassProfiler::NoteExternal(External::Reproject, ms(1, 2));
        GpuPassProfiler::NoteExternal(External::Warp, ms(2, 3));
        GpuPassProfiler::NoteExternal(External::EyeTaa, ms(3, 4));
        GpuPassProfiler::NoteExternal(External::Upscale, ms(4, 5));
        GpuPassProfiler::NoteExternal(External::Copy, ms(5, 6));
    }
}

void VrLateWarp::BeginFrame() {
    ++frame;
    if (!IsActive() || swapchain == XR_NULL_HANDLE || !acquired || waited) {
        return;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = WaitImageTimeoutNs;
    const XrResult r = xrWaitSwapchainImage(swapchain, &wait);
    if (r == XR_TIMEOUT_EXPIRED) {
        ++stats.wait_timeouts;
        return; // still acquired: waited again next frame, never released unwaited
    }
    if (XR_FAILED(r)) {
        Disable(fmt::format("xrWaitSwapchainImage failed ({})", static_cast<s32>(r)));
        return;
    }
    waited = true;
}

void VrLateWarp::FillViews(std::array<XrCompositionLayerProjectionView, 2>& views) const {
    for (u32 i = 0; i < 2; ++i) {
        XrCompositionLayerProjectionView& view = views[i];
        view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose = shown_poses[i];
        view.fov = shown_fovs[i];
        view.subImage.swapchain = swapchain;
        view.subImage.imageRect = shown_rects[i];
        view.subImage.imageArrayIndex = 0;
    }
}

bool VrLateWarp::Reuse(std::array<XrCompositionLayerProjectionView, 2>& views) {
    if (!IsActive() || !has_image || swapchain == XR_NULL_HANDLE ||
        frame - shown_frame > ReuseFrames) {
        return false;
    }
    ++stats.reused;
    FillViews(views);
    return true;
}

bool VrLateWarp::Render(const Source& source,
                        std::array<XrCompositionLayerProjectionView, 2>& views) {
    taa_used_history = false;
    if (!IsActive()) {
        return false;
    }
    const auto& cap = source.capture;
    if (!cap.valid || cap.size.width == 0 || cap.size.height == 0) {
        return Reuse(views);
    }
    for (const vk::Rect2D& r : source.crops) {
        if (r.extent.width == 0 || r.extent.height == 0 || r.offset.x < 0 || r.offset.y < 0 ||
            r.offset.x + r.extent.width > cap.size.width ||
            r.offset.y + r.extent.height > cap.size.height) {
            return Reuse(views);
        }
    }
    // eye_upscale / eye_sharpen: each eye at its output size, enlarged and sharpened from the
    // warp's (VrEyeUpscale); else as warped, at the render's pixels.
    const std::array<vk::Extent2D, 2> in{source.crops[0].extent, source.crops[1].extent};
    std::array<vk::Extent2D, 2> out = in;
    bool scaling = false;
    const float output_gamma = cap.settings.gamma > 0.0f ? cap.settings.gamma : 1.0f;
    if (upscale.Ready()) {
        bool sized = true;
        for (u32 e = 0; e < 2; ++e) {
            sized = sized && source.out[e].width >= in[e].width &&
                    source.out[e].height >= in[e].height;
        }
        if (sized && (source.out != in || source.sharpen > 0.0f || source.color.Active() ||
                      output_gamma != 1.0f ||
                      source.vignettes.health > .0001f || source.vignettes.desaturation > .0001f)) {
            scaling = upscale.Prepare(in, source.out, source.sharpen);
            if (scaling) {
                out = source.out;
            }
        }
    }
    if (!scaling && (source.color.Active() || output_gamma != 1.0f)) {
        return false;
    }
    const vk::Extent2D both{out[0].width + out[1].width, std::max(out[0].height, out[1].height)};
    if (!EnsureSwapchain(both) ||
        !EnsureImage(eyes, both, vk::Format::eR8G8B8A8Unorm,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                     "eyes") ||
        !EnsureImage(q_map, cap.size, vk::Format::eR32G32Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferDst,
                     "map") ||
        !EnsureImage(v_disp, cap.size, vk::Format::eR32G32Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferDst,
                     "disparity") ||
        !EnsureImage(v_tiles,
                     {DivCeil(cap.size.width, VrStereo::kTileSize),
                      DivCeil(cap.size.height, VrStereo::kTileSize)},
                     vk::Format::eR32G32B32A32Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferDst,
                     "tiles")) {
        return false;
    }
    // eye_taa: each eye as warped (eye_src), blended with its history (taa_hist) into the next
    // history, which goes to the upscale or into `eyes`.
    const bool taa_on = source.taa > 0.0f && static_cast<bool>(taa_pipeline);
    const bool per_eye = scaling || taa_on;
    for (u32 e = 0; e < 2 && per_eye; ++e) {
        if (!EnsureImage(eye_src[e], in[e], vk::Format::eR8G8B8A8Unorm,
                         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                         e == 0 ? "left eye" : "right eye")) {
            return false;
        }
    }
    if (taa_on) {
        for (u32 e = 0; e < 2; ++e) {
            for (Image& h : taa_hist[e]) {
                const vk::Image before = h.image;
                if (!EnsureImage(h, in[e], vk::Format::eR16G16B16A16Sfloat,
                                 vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                 "eye history")) {
                    return false;
                }
                if (h.image != before) {
                    taa_hist_ready = false;
                    taa_valid = false;
                }
            }
        }
    } else {
        taa_valid = false;
    }
    // eye_taa: the history this warp blends, against the last rendered warp's (VrEyeTaa::Choose),
    // and whether the hunter's body is classified: late_warp_motion, or the TAA while the world
    // moves (its body, held items: no history then).
    taa_choice = taa_on ? VrEyeTaa::Choose(source.taa_follow, taa_valid, taa_prev_frame,
                                           source.taa_frame)
                        : VrEyeTaa::Choice{};
    mask_body = source.motion || (taa_on && taa_choice.moving);
    if (!EnsureImage(eye_w,
                     taa_on ? vk::Extent2D{in[0].width + in[1].width,
                                           std::max(in[0].height, in[1].height)}
                            : (eye_w.image ? eye_w.extent : vk::Extent2D{1, 1}),
                     vk::Format::eR32Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                     "eye 1/z")) {
        return false;
    }
    if (mask_body) {
        const vk::Extent2D grid{DivCeil(cap.size.width, VrStereo::kTileSize),
                                DivCeil(cap.size.height, VrStereo::kTileSize)};
        if (signed_w.extent != cap.size || layer_tiles.extent != grid) {
            layers_ready = false;
            classified_id = 0;
        }
        if (!EnsureImage(signed_w, cap.size, vk::Format::eR32Sfloat,
                         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                         "signed 1/z") ||
            !EnsureImage(layer_tiles, grid, vk::Format::eR32G32B32A32Sfloat,
                         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                         "layer tiles")) {
            return false;
        }
    }
    Slot& slot = slots[slot_index];
    if (slot.pending) {
        const vk::Result status = dev.device.getFenceStatus(slot.fence);
        if (status == vk::Result::eNotReady) {
            ++stats.gpu_late; // the warp of two XR frames ago is still running
            return Reuse(views);
        }
        if (status != vk::Result::eSuccess) {
            VrDeviceLost::Note(status, "the late warp's fence");
            Disable(fmt::format("fence status {}", vk::to_string(status)));
            return false;
        }
        ReadTimestamps(slot_index);
        slot.pending = false;
        if (dev.device.resetFences(slot.fence) != vk::Result::eSuccess) {
            Disable("fence reset");
            return false;
        }
    }
    if (!acquired && !AcquireImage()) {
        return false;
    }
    if (!waited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = WaitImageTimeoutNs;
        XrResult r = XR_SUCCESS;
        {
            LockTrace::Phase phase{"late warp: xrWaitSwapchainImage"}; // VR_LOCK_TRACE only
            r = xrWaitSwapchainImage(swapchain, &wait);
        }
        if (r == XR_TIMEOUT_EXPIRED) {
            ++stats.wait_timeouts;
            return Reuse(views);
        }
        if (XR_FAILED(r)) {
            Disable(fmt::format("xrWaitSwapchainImage failed ({})", static_cast<s32>(r)));
            return false;
        }
        waited = true;
    }
    bool recorded = false;
    {
        LockTrace::Phase phase{"late warp: record"}; // VR_LOCK_TRACE only
        recorded = Record(slot, slot_index, source, scaling, out);
    }
    if (!recorded) {
        return false;
    }
    // The runtime's own queue (SetQueueHooks): this batch also waits for the runtime's work on the
    // acquired image and signals the timeline the XR queue waits for before the release.
    const QueueSync queue_sync =
        queue_hooks.before_submit ? queue_hooks.before_submit() : QueueSync{};
    std::array<vk::Semaphore, 2> wait_semaphores{};
    std::array<u64, 2> wait_values{};
    std::array<vk::PipelineStageFlags, 2> wait_stages{};
    u32 waits = 0;
    if (source.gpu) {
        // Waits for the capture's draw-stream work (done in practice) before reading it.
        wait_semaphores[waits] = source.gpu;
        wait_values[waits] = source.gpu_tick;
        wait_stages[waits] = vk::PipelineStageFlagBits::eComputeShader;
        ++waits;
    }
    if (queue_sync.wait) {
        wait_semaphores[waits] = queue_sync.wait;
        wait_values[waits] = queue_sync.wait_value;
        wait_stages[waits] = vk::PipelineStageFlagBits::eAllCommands;
        ++waits;
    }
    // VR_LATENCY_GPU: also signals the presenter's warp-done timeline (SetDoneSemaphore).
    const bool signal_done = static_cast<bool>(done_semaphore);
    const u64 done_next = done_value + 1;
    std::array<vk::Semaphore, 2> signal_semaphores{};
    std::array<u64, 2> signal_values{};
    u32 signals = 0;
    if (signal_done) {
        signal_semaphores[signals] = done_semaphore;
        signal_values[signals] = done_next;
        ++signals;
    }
    if (queue_sync.signal) {
        signal_semaphores[signals] = queue_sync.signal;
        signal_values[signals] = queue_sync.signal_value;
        ++signals;
    }
    const vk::TimelineSemaphoreSubmitInfo timeline{
        .waitSemaphoreValueCount = waits,
        .pWaitSemaphoreValues = wait_values.data(),
        .signalSemaphoreValueCount = signals,
        .pSignalSemaphoreValues = signal_values.data(),
    };
    const vk::SubmitInfo submit{
        .pNext = waits != 0 || signals != 0 ? &timeline : nullptr,
        .waitSemaphoreCount = waits,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &slot.cmd,
        .signalSemaphoreCount = signals,
        .pSignalSemaphores = signal_semaphores.data(),
    };
    vk::Result submitted = vk::Result::eSuccess;
    {
        LockTrace::Phase phase{"late warp: vkQueueSubmit"}; // VR_LOCK_TRACE only
        submitted = dev.queue.submit(submit, slot.fence);
    }
    if (submitted != vk::Result::eSuccess) {
        VrDeviceLost::Note(submitted, "the late warp's submit");
        Disable(fmt::format("queue submit: {}", vk::to_string(submitted)));
        return false;
    }
    if (signal_done) {
        done_value = done_next;
    }
    slot.pending = true;
    if (queue_hooks.after_submit) {
        queue_hooks.after_submit(queue_sync); // the runtime's own queue: it waits for this warp
    }
    // After the submit on the session's queue: the runtime sees the warp complete.
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult released = XR_SUCCESS;
    {
        LockTrace::Phase phase{"late warp: xrReleaseSwapchainImage"}; // VR_LOCK_TRACE only
        released = xrReleaseSwapchainImage(swapchain, &release);
    }
    if (XR_FAILED(released)) {
        Disable(fmt::format("xrReleaseSwapchainImage failed ({})", static_cast<s32>(released)));
        return false;
    }
    acquired = waited = false;
    has_image = true;
    shown_frame = frame;
    shown_poses = source.eye_poses;
    shown_fovs = source.fovs;
    shown_rects[0] = {{0, 0},
                      {static_cast<s32>(out[0].width), static_cast<s32>(out[0].height)}};
    shown_rects[1] = {{static_cast<s32>(out[0].width), 0},
                      {static_cast<s32>(out[1].width), static_cast<s32>(out[1].height)}};
    slot_upscaled[slot_index] = scaling;
    slot_taa[slot_index] = taa_on;
    if (taa_on) {
        stats.taa += 1;
        stats.taa_history += taa_used_history ? 1u : 0u;
        stats.taa_moving += taa_used_history && taa_choice.moving ? 1u : 0u;
        stats.taa_head +=
            taa_used_history && taa_choice.history == VrEyeTaa::History::Head ? 1u : 0u;
        taa_parity ^= 1;
        taa_valid = true;
        taa_prev_poses = source.eye_poses;
        taa_prev_views = source.taa_views;
        taa_prev_frame = source.taa_frame;
        taa_prev_size = in;
    }
    slot_index ^= 1;
    ++stats.rendered;
    stats.upscaled += scaling ? 1u : 0u;
    stats.out = out;
    stats.motion += source.motion ? 1u : 0u;
    // The next image now, while the queue lock is held anyway; BeginFrame waits for it.
    AcquireImage();
    FillViews(views);
    return true;
}

bool VrLateWarp::Record(Slot& slot, u32 index, const Source& source, bool scaling,
                        const std::array<vk::Extent2D, 2>& out) {
    const bool taa_on = source.taa > 0.0f && static_cast<bool>(taa_pipeline);
    const bool per_eye = scaling || taa_on;
    const auto& cap = source.capture;
    const auto& s = cap.settings;
    const auto& d = cap.disparity;
    const VrLate::Intrinsics in = VrLate::MakeIntrinsics(
        cap.camera.vfov, cap.camera.aspect, static_cast<float>(cap.size.width),
        static_cast<float>(cap.size.height));
    const s32 width = static_cast<s32>(cap.size.width);
    const s32 height = static_cast<s32>(cap.size.height);

    // ---- pass 1 constants: the V pixels both eyes' scans can read (the crops, a workgroup and
    // the reach beyond them sideways), in whole tiles ----
    const s32 reach = static_cast<s32>(std::ceil(cap.reach_px)) + 72;
    s32 x0 = width, x1 = 0, y0 = height, y1 = 0;
    for (const vk::Rect2D& r : source.crops) {
        x0 = std::min(x0, r.offset.x - reach);
        x1 = std::max(x1, r.offset.x + static_cast<s32>(r.extent.width) + reach);
        y0 = std::min(y0, r.offset.y - 8);
        y1 = std::max(y1, r.offset.y + static_cast<s32>(r.extent.height) + 8);
    }
    x0 = std::max(x0, 0) & ~15;
    y0 = std::max(y0, 0) & ~15;
    x1 = std::min((x1 + 15) & ~15, (width + 15) & ~15);
    y1 = std::min((y1 + 15) & ~15, (height + 15) & ~15);
    ReprojectConstants rc{};
    const auto& rsv = source.reprojection.rsv;
    for (int i = 0; i < 3; ++i) {
        rc.rsv[i][0] = rsv.m[i][0];
        rc.rsv[i][1] = rsv.m[i][1];
        rc.rsv[i][2] = rsv.m[i][2];
    }
    const auto& c = source.reprojection.c;
    rc.c[0] = c.x;
    rc.c[1] = c.y;
    rc.c[2] = c.z;
    // The 1/z edge: the game-rate warp's tau, in disparity px, at the capture's K.
    rc.c[3] = d.k_px > 0.0f ? s.tau_px / d.k_px : s.tau_px / 12.0f;
    rc.intr[0] = in.fx;
    rc.intr[1] = in.fy;
    rc.intr[2] = in.cx;
    rc.intr[3] = in.cy;
    rc.region[0] = x0;
    rc.region[1] = y0;
    rc.region[2] = x1;
    rc.region[3] = y1;
    rc.size[0] = width;
    rc.size[1] = height;
    rc.size[2] = static_cast<s32>(DivCeil(cap.size.width, VrStereo::kTileSize));
    rc.size[3] = static_cast<s32>(DivCeil(cap.size.height, VrStereo::kTileSize));
    rc.disp[0] = d.k_px;
    rc.disp[1] = d.knee_px;
    rc.disp[2] = d.floor_px;
    rc.disp[3] = d.max_px;
    rc.misc[0] = d.snap_px;
    rc.misc[1] = cap.camera.near_z > 0.0f ? 1.0f / cap.camera.near_z : 10.0f;
    rc.misc[2] = VrLate::kStepPx;
    rc.misc[3] = static_cast<float>(VrLate::kMaxSteps);
    const auto& rw = source.world.rsv;
    for (int i = 0; i < 3; ++i) {
        rc.rwv[i][0] = rw.m[i][0];
        rc.rwv[i][1] = rw.m[i][1];
        rc.rwv[i][2] = rw.m[i][2];
    }
    rc.cw[0] = source.world.c.x;
    rc.cw[1] = source.world.c.y;
    rc.cw[2] = source.world.c.z;
    // VR_LATE_DEBUG_BODY=1 (with dibr_debug_holes=1): the hunter's body pixels in the holes' green.
    static const bool debug_body = std::getenv("VR_LATE_DEBUG_BODY") != nullptr;
    rc.cw[3] = static_cast<float>((source.motion ? 1 : 0) |
                                  (source.motion && debug_body ? 2 : 0) | (mask_body ? 4 : 0));
    u8* const slot_uniforms = static_cast<u8*>(uniforms.mapped) + index * uniform_stride;
    std::memcpy(slot_uniforms, &rc, sizeof(rc));
    // late_warp_motion: the capture's texels classified into the hunter's body and the world,
    // once per capture (and body).
    const bool classify = mask_body && classified_id != source.capture_id;
    if (classify) {
        ClassifyConstants cc{};
        cc.intr[0] = in.fx;
        cc.intr[1] = in.fy;
        cc.intr[2] = in.cx;
        cc.intr[3] = in.cy;
        cc.size[0] = width;
        cc.size[1] = height;
        cc.size[2] = static_cast<s32>(std::min(source.body.count, VrLate::kMaxBodyCapsules));
        cc.reach[0] = source.body.reach > 0.0f ? 1.0f / source.body.reach : 3.0e38f;
        cc.reach[1] = cap.item_mask ? 1.0f : 0.0f; // our in-frame items are body
        cc.floor_plane[0] = source.body.floor_n.x;
        cc.floor_plane[1] = source.body.floor_n.y;
        cc.floor_plane[2] = source.body.floor_n.z;
        cc.floor_plane[3] = source.body.floor_d;
        for (u32 i = 0; i < static_cast<u32>(cc.size[2]); ++i) {
            const VrLate::Capsule& k = source.body.capsules[i];
            cc.caps[2 * i][0] = k.a.x;
            cc.caps[2 * i][1] = k.a.y;
            cc.caps[2 * i][2] = k.a.z;
            cc.caps[2 * i][3] = k.radius;
            cc.caps[2 * i + 1][0] = k.b.x;
            cc.caps[2 * i + 1][1] = k.b.y;
            cc.caps[2 * i + 1][2] = k.b.z;
            cc.caps[2 * i + 1][3] = k.behind;
        }
        std::memcpy(slot_uniforms + classify_offset, &cc, sizeof(cc));
    }
    // vignette_in_warp (VrVignetteWarp): the frame's vignettes, blended into each eye by the
    // upscale's last pass, for exactly the eye poses and FOVs this pair is submitted with.
    const bool vignettes = scaling && upscale.VignettesReady() &&
                           (source.vignettes.count > 0 || source.vignettes.health > .0001f ||
                            source.vignettes.desaturation > .0001f);
    if (vignettes) {
        VrVignetteWarp::Block vb{};
        vb.head = {std::min(source.vignettes.count, VrVignetteWarp::MaxItems), bgra ? 1u : 0u,
                   std::bit_cast<u32>(source.vignettes.health),
                   std::bit_cast<u32>(source.vignettes.desaturation)};
        vb.field = source.vignettes.field;
        for (u32 e = 0; e < 2; ++e) {
            const XrPosef& pose = source.eye_poses[e];
            const VrLate::Mat3 r = VrLate::FromQuat(
                {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w});
            for (u32 col = 0; col < 3; ++col) {
                vb.eye_rot[3 * e + col] = {r.m[0][col], r.m[1][col], r.m[2][col], 0.0f};
            }
            vb.eye_pos[e] = {pose.position.x, pose.position.y, pose.position.z, 0.0f};
            const XrFovf& f = source.fovs[e];
            vb.tan[e] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp),
                         std::tan(f.angleDown)};
        }
        for (u32 i = 0; i < vb.head[0]; ++i) {
            vb.items[i] = source.vignettes.items[i];
        }
        std::memcpy(slot_uniforms + vignette_offset, &vb, sizeof(vb));
    }
    vmaFlushAllocation(dev.allocator, uniforms.allocation, index * uniform_stride,
                       uniform_stride);

    // ---- pass 2 constants: vr_dibr_warp.comp's, both eyes side by side in `eyes` ----
    VrStereo::WarpPush push{};
    u32 flags = 0;
    if (s.catmull_rom) {
        flags |= VrStereo::FlagCatmullRom;
    }
    if (s.debug_holes) {
        flags |= VrStereo::FlagDebugHoles;
    }
    if (s.debug_hud) {
        flags |= VrStereo::FlagDebugHud;
    }
    if (cap.scene_decode) {
        flags |= VrStereo::FlagDecodeScene;
    }
    if (cap.hud) {
        flags |= VrStereo::FlagHudDiff | (cap.hud_decode ? VrStereo::FlagDecodeHud : 0u);
        if (cap.item_mask) {
            flags |= VrStereo::FlagItemMask;
        }
    }
    if (bgra) {
        flags |= VrStereo::FlagBgra0 | VrStereo::FlagBgra1;
    }
    if (taa_on) {
        flags |= VrStereo::FlagEyeW | (source.taa_follow ? VrStereo::FlagEyeWView : 0u) |
                 (taa_choice.moving ? VrStereo::FlagEyeWMoving : 0u);
    }
    const float hud_depth_units = s.hud_depth_m * cap.camera.units_per_metre;
    u32 max_w = 0, max_h = 0;
    for (u32 e = 0; e < 2; ++e) {
        const vk::Rect2D& r = source.crops[e];
        push.crop[e][0] = r.offset.x;
        push.crop[e][1] = r.offset.y;
        push.crop[e][2] = static_cast<s32>(r.extent.width);
        push.crop[e][3] = static_cast<s32>(r.extent.height);
        // Upscaling or eye_taa: each eye into its own image (the upscaler's per-eye clamp), else
        // side by side into `eyes`.
        push.dst[e][0] = e == 0 || per_eye ? 0 : static_cast<s32>(source.crops[0].extent.width);
        push.dst[e][1] = 0;
        push.dst[e][2] = VrStereo::HudShiftPx(d.k_px, hud_depth_units, static_cast<int>(e));
        max_w = std::max(max_w, r.extent.width);
        max_h = std::max(max_h, r.extent.height);
    }
    push.src_size[0] = width;
    push.src_size[1] = height;
    push.tau = s.tau_px;
    push.reach = cap.reach_px;
    push.hud_thresh = s.hud_threshold;
    push.gamma = 1.0f;
    push.flags = flags;
    push.first_eye = 0;
    push.knee_px = d.knee_px;
    push.knee_r = d.floor_px > d.knee_px ? d.floor_px - d.knee_px : 0.0f;
    push.k_px = d.k_px;
    // FlagEyeW (LATE_WARP has no depth buffer): eye 1's first column in eye_w.
    push.depth_base1 = taa_on ? source.crops[0].extent.width : 0u;
    // late_warp_edge_blend: depth edges whose sides move apart by less than the threshold between
    // the capture and V blended rather than the nearest texel (off while the world moves).
    push.edge_k = VrLate::EdgeBlendScale(source.edge_blend_px, source.motion, source.reprojection,
                                         in, static_cast<float>(width),
                                         static_cast<float>(height));
    if (source.edge_blend_px != edge_blend_logged) {
        edge_blend_logged = source.edge_blend_px;
        LOG_INFO(Render_Vulkan,
                 "VR late warp: depth-edge colour {} (late_warp_edge_blend={}){}",
                 source.edge_blend_px > 0.0f ? "blended below the parallax threshold"
                                             : "the nearest texel",
                 source.edge_blend_px,
                 source.edge_blend_px > 0.0f
                     ? fmt::format("; this warp: {:.2f} px per unit of 1/z (motion {})",
                                   VrLate::ParallaxPerInverseDepth(source.reprojection, in,
                                                                   static_cast<float>(width),
                                                                   static_cast<float>(height)),
                                   source.motion ? "on: off for it" : "off")
                     : std::string{});
    }

    // ---- commands ----
    const vk::CommandBuffer cmd = slot.cmd;
    if (cmd.reset() != vk::Result::eSuccess ||
        cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) !=
            vk::Result::eSuccess) {
        Disable("command buffer begin");
        return false;
    }
    const u32 q0 = index * QueriesPerSlot;
    if (query_pool) {
        cmd.resetQueryPool(query_pool, q0, QueriesPerSlot);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, query_pool, q0);
    }
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    using L = vk::ImageLayout;
    // V's images: overwritten (after our previous frame's warp read them). New ones are cleared
    // once, so columns no workgroup writes read as sky; afterwards they keep their contents (from
    // the layout the previous frame left them in: every recorded frame is submitted, a failure
    // turns the late warp off), so a column this frame's pass 1 skips still reads as before.
    const bool fresh = !q_map_cleared;
    const L kept = fresh ? L::eUndefined : L::eShaderReadOnlyOptimal;
    std::array<vk::ImageMemoryBarrier2, 7> pre{
        Barrier(q_map.image, kept, fresh ? L::eTransferDstOptimal : L::eGeneral,
                S::eComputeShader, A::eNone, fresh ? S::eClear : S::eComputeShader,
                fresh ? A::eTransferWrite : A::eShaderStorageWrite),
        Barrier(v_disp.image, kept, fresh ? L::eTransferDstOptimal : L::eGeneral,
                S::eComputeShader, A::eNone, fresh ? S::eClear : S::eComputeShader,
                fresh ? A::eTransferWrite : A::eShaderStorageWrite),
        Barrier(v_tiles.image, kept, fresh ? L::eTransferDstOptimal : L::eGeneral,
                S::eComputeShader, A::eNone, fresh ? S::eClear : S::eComputeShader,
                fresh ? A::eTransferWrite : A::eShaderStorageWrite),
        // Both eyes: the previous frame's copy out of it is done.
        Barrier(eyes.image, L::eUndefined, L::eGeneral, S::eCopy, A::eNone, S::eComputeShader,
                A::eShaderStorageWrite),
        // Upscaling: each eye as warped (the previous frame's upscale read them).
        Barrier(eye_src[0].image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eNone,
                S::eComputeShader, A::eShaderStorageWrite),
        Barrier(eye_src[1].image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eNone,
                S::eComputeShader, A::eShaderStorageWrite),
        // The eyes' 1/z (eye_taa): the previous frame's TAA read it.
        Barrier(eye_w.image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eNone,
                S::eComputeShader, A::eShaderStorageWrite),
    };
    if (!per_eye) {
        pre[4] = pre[6];
    }
    cmd.pipelineBarrier2(
        {.imageMemoryBarrierCount = per_eye ? 7u : 5u, .pImageMemoryBarriers = pre.data()});
    if (taa_on) {
        // The histories: the one this warp writes (read two warps ago) and the one it reads
        // (written by the last warp); new ones from UNDEFINED once.
        const u32 w = taa_parity;
        const L from = taa_hist_ready ? L::eGeneral : L::eUndefined;
        std::array<vk::ImageMemoryBarrier2, 4> hist{};
        for (u32 e = 0; e < 2; ++e) {
            hist[2 * e] = Barrier(taa_hist[e][w].image, from, L::eGeneral, S::eComputeShader,
                                  A::eNone, S::eComputeShader, A::eShaderStorageWrite);
            hist[2 * e + 1] = Barrier(taa_hist[e][w ^ 1].image, from, L::eGeneral,
                                      S::eComputeShader, A::eShaderStorageWrite,
                                      S::eComputeShader, A::eShaderSampledRead);
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = hist.data()});
        if (!taa_hist_ready) {
            taa_valid = false; // just created: nothing to blend
            taa_hist_ready = true;
        }
    }
    if (fresh) {
        const vk::ClearColorValue zero{.float32 = std::array{0.0f, 0.0f, 0.0f, 0.0f}};
        for (const Image* image : {&q_map, &v_disp, &v_tiles}) {
            cmd.clearColorImage(image->image, L::eTransferDstOptimal, zero, ColorRange);
        }
        std::array<vk::ImageMemoryBarrier2, 3> cleared{};
        const std::array<vk::Image, 3> images{q_map.image, v_disp.image, v_tiles.image};
        for (u32 i = 0; i < 3; ++i) {
            cleared[i] = Barrier(images[i], L::eTransferDstOptimal, L::eGeneral, S::eClear,
                                 A::eTransferWrite, S::eComputeShader,
                                 A::eShaderStorageWrite | A::eShaderStorageRead);
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = cleared.data()});
        q_map_cleared = true;
    }

    // late_warp_motion: classify the capture's texels (body / world) once per capture. Its two
    // images stay in GENERAL; the barrier orders this after every earlier submission's reads.
    const bool layers = mask_body && signed_w.image && layer_tiles.image;
    if (mask_body && !layers_ready) {
        const std::array<vk::ImageMemoryBarrier2, 2> init{
            Barrier(signed_w.image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eNone,
                    S::eComputeShader, A::eShaderStorageWrite),
            Barrier(layer_tiles.image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eNone,
                    S::eComputeShader, A::eShaderStorageWrite),
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = init.data()});
        layers_ready = true;
    }
    if (classify) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = S::eComputeShader,
            .srcAccessMask = A::eNone,
            .dstStageMask = S::eComputeShader,
            .dstAccessMask = A::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const std::array<vk::DescriptorImageInfo, 4> images{{
            {.imageView = cap.inverse_depth, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = signed_w.view, .imageLayout = L::eGeneral},
            {.imageView = layer_tiles.view, .imageLayout = L::eGeneral},
            // No item mask: S's 1/z stands in (not read).
            {.imageView = cap.item_mask ? cap.item_mask : cap.inverse_depth,
             .imageLayout = L::eShaderReadOnlyOptimal},
        }};
        const vk::DescriptorBufferInfo ubo{
            .buffer = uniforms.buffer,
            .offset = index * uniform_stride + classify_offset,
            .range = sizeof(ClassifyConstants),
        };
        std::array<vk::WriteDescriptorSet, 5> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType =
                    i == 0 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage,
                .pImageInfo = &images[i],
            };
        }
        writes[3] = {
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &ubo,
        };
        writes[4] = {
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &images[3],
        };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, classify_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, classify_layout, 0, writes);
        cmd.dispatch(DivCeil(cap.size.width, 16), DivCeil(cap.size.height, 16), 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = S::eComputeShader,
            .srcAccessMask = A::eShaderStorageWrite,
            .dstStageMask = S::eComputeShader,
            .dstAccessMask = A::eShaderSampledRead,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        classified_id = source.capture_id;
        ++stats.classified;
    }
    slot_classified[index] = classify;
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, query_pool, q0 + 1);
    }

    // Pass 1.
    {
        // Without motion or TAA body masking the capture's images stand in (not read as layers).
        const std::array<vk::DescriptorImageInfo, 7> images{{
            {.imageView = cap.inverse_depth, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = cap.tiles, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = q_map.view, .imageLayout = L::eGeneral},
            {.imageView = v_disp.view, .imageLayout = L::eGeneral},
            {.imageView = v_tiles.view, .imageLayout = L::eGeneral},
            layers ? vk::DescriptorImageInfo{.imageView = signed_w.view,
                                             .imageLayout = L::eGeneral}
                   : vk::DescriptorImageInfo{.imageView = cap.inverse_depth,
                                             .imageLayout = L::eShaderReadOnlyOptimal},
            layers ? vk::DescriptorImageInfo{.imageView = layer_tiles.view,
                                             .imageLayout = L::eGeneral}
                   : vk::DescriptorImageInfo{.imageView = cap.tiles,
                                             .imageLayout = L::eShaderReadOnlyOptimal},
        }};
        const vk::DescriptorBufferInfo ubo{
            .buffer = uniforms.buffer,
            .offset = index * uniform_stride,
            .range = sizeof(ReprojectConstants),
        };
        std::array<vk::WriteDescriptorSet, 8> writes{};
        const std::array<u32, 7> image_bindings{0, 1, 2, 3, 4, 6, 7};
        for (u32 i = 0; i < 7; ++i) {
            const u32 b = image_bindings[i];
            writes[i] = {
                .dstBinding = b,
                .descriptorCount = 1,
                .descriptorType = b >= 2 && b <= 4 ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .pImageInfo = &images[i],
            };
        }
        writes[7] = {
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &ubo,
        };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, reproject_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, reproject_layout, 0, writes);
        cmd.dispatch(static_cast<u32>(x1 - x0) / 16, static_cast<u32>(y1 - y0) / 16, 1);
    }
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, query_pool, q0 + 2);
    }
    std::array<vk::ImageMemoryBarrier2, 3> mid{};
    const std::array<vk::Image, 3> v_images{q_map.image, v_disp.image, v_tiles.image};
    for (u32 i = 0; i < 3; ++i) {
        mid[i] = Barrier(v_images[i], L::eGeneral, L::eShaderReadOnlyOptimal, S::eComputeShader,
                         A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead);
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = mid.data()});

    // Pass 2.
    {
        const vk::ImageView hud_view = cap.hud ? cap.hud : cap.scene;
        const vk::ImageView mask_view = cap.item_mask ? cap.item_mask : cap.scene;
        const std::array<vk::DescriptorImageInfo, 10> images{{
            {.imageView = v_disp.view, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = v_tiles.view, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = cap.scene, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = hud_view, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = per_eye ? eye_src[0].view : eyes.view, .imageLayout = L::eGeneral},
            {.imageView = per_eye ? eye_src[1].view : eyes.view, .imageLayout = L::eGeneral},
            {.imageView = mask_view, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = q_map.view, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = cap.inverse_depth, .imageLayout = L::eShaderReadOnlyOptimal},
            {.imageView = eye_w.view, .imageLayout = L::eGeneral},
        }};
        const vk::DescriptorBufferInfo depth{
            .buffer = dummy.buffer,
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };
        std::array<vk::WriteDescriptorSet, 11> writes{};
        const std::array<u32, 10> image_bindings{0, 1, 2, 3, 4, 5, 7, 8, 9, 10};
        for (u32 i = 0; i < 10; ++i) {
            const u32 b = image_bindings[i];
            writes[i] = {
                .dstBinding = b,
                .descriptorCount = 1,
                .descriptorType = b == 4 || b == 5 || b == 10 ? vk::DescriptorType::eStorageImage
                                                              : vk::DescriptorType::eSampledImage,
                .pImageInfo = &images[i],
            };
        }
        writes[10] = {
            .dstBinding = 6,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &depth,
        };
        const bool once = source.fetch_once && static_cast<bool>(warp_pipeline_once);
        stats.fetch_once += once ? 1u : 0u;
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, once ? warp_pipeline_once : warp_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, warp_layout, 0, writes);
        ScratchPush::Invalidate();
        cmd.pushConstants(warp_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch(DivCeil(max_w, static_cast<u32>(VrStereo::kWarpGroupWidth)),
                     DivCeil(max_h, static_cast<u32>(VrStereo::kWarpGroupRows)), 2);
    }
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, query_pool, q0 + 3);
    }
    if (per_eye) {
        const std::array<vk::ImageMemoryBarrier2, 3> warped{
            Barrier(eye_src[0].image, L::eGeneral, L::eGeneral, S::eComputeShader,
                    A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead),
            Barrier(eye_src[1].image, L::eGeneral, L::eGeneral, S::eComputeShader,
                    A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead),
            Barrier(eye_w.image, L::eGeneral, L::eGeneral, S::eComputeShader,
                    A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead),
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = taa_on ? 3u : 2u,
                              .pImageMemoryBarriers = warped.data()});
    }
    // eye_taa: each eye blended with history moved by head AND game-camera motion.
    taa_used_history = false;
    if (taa_on) {
        static const bool taa_debug = std::getenv("VR_TAA_DEBUG") != nullptr;
        const u32 w = taa_parity;
        const float mpu = cap.camera.units_per_metre > 0.0f ? 1.0f / cap.camera.units_per_metre
                                                            : 1.0f;
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, taa_pipeline);
        for (u32 e = 0; e < 2; ++e) {
            const vk::Rect2D& r = source.crops[e];
            const std::array<float, 4> intr{in.fx, in.fy, in.cx - static_cast<float>(r.offset.x),
                                            in.cy - static_cast<float>(r.offset.y)};
            const bool history = taa_valid && taa_choice.history != VrEyeTaa::History::None &&
                                 taa_prev_size[e] == r.extent &&
                                 std::abs(taa_prev_intr[e][0] - intr[0]) <= 1e-3f * intr[0] &&
                                 std::abs(taa_prev_intr[e][1] - intr[1]) <= 1e-3f * intr[1];
            taa_used_history = taa_used_history || history;
            TaaPush tp{};
            VrEyeTaa::Rigid rigid{};
            if (taa_choice.history == VrEyeTaa::History::World) {
                rigid = VrEyeTaa::Between(taa_prev_views[e], source.taa_views[e]);
            } else {
                // The tracked eye poses (the original eye TAA): the world stood still.
                const auto rot = [](const XrQuaternionf& q) {
                    return VrEyeTaa::Rotation(q.x, q.y, q.z, q.w);
                };
                const XrVector3f& pc = source.eye_poses[e].position;
                const XrVector3f& pp = taa_prev_poses[e].position;
                rigid = VrEyeTaa::Between(rot(taa_prev_poses[e].orientation), {pp.x, pp.y, pp.z},
                                          rot(source.eye_poses[e].orientation), {pc.x, pc.y, pc.z});
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    tp.m[i][j] = rigid.m[i][j];
                }
                tp.t[i] = rigid.t[i];
            }
            tp.m[0][3] = std::clamp(1.0f - source.taa, 0.05f, 1.0f);
            tp.m[1][3] = mpu;
            tp.m[2][3] = source.taa_gate_px;
            tp.t[3] = source.taa_edge_cap;
            for (int i = 0; i < 4; ++i) {
                tp.intr[i] = intr[i];
                tp.intr_p[i] = taa_prev_intr[e][i];
            }
            tp.size[0] = static_cast<s32>(r.extent.width);
            tp.size[1] = static_cast<s32>(r.extent.height);
            tp.size[2] = static_cast<s32>(std::max(taa_prev_size[e].width, 1u));
            tp.size[3] = static_cast<s32>(std::max(taa_prev_size[e].height, 1u));
            tp.misc[0] = e == 0 ? 0 : static_cast<s32>(source.crops[0].extent.width);
            tp.misc[1] = e == 0 ? 0 : static_cast<s32>(out[0].width);
            tp.misc[2] = 0;
            tp.misc[3] = (history ? TaaHistory : 0) | (scaling ? 0 : TaaWrite8) |
                         (taa_debug ? TaaDebug : 0) | (taa_choice.moving ? TaaMoving : 0);
            const std::array<vk::DescriptorImageInfo, 5> images{{
                {.imageView = eye_src[e].view, .imageLayout = L::eGeneral},
                {.imageView = taa_hist[e][w ^ 1].view, .imageLayout = L::eGeneral},
                {.imageView = eye_w.view, .imageLayout = L::eGeneral},
                {.imageView = taa_hist[e][w].view, .imageLayout = L::eGeneral},
                {.imageView = eyes.view, .imageLayout = L::eGeneral},
            }};
            std::array<vk::WriteDescriptorSet, 5> writes{};
            for (u32 i = 0; i < 5; ++i) {
                writes[i] = {
                    .dstBinding = i,
                    .descriptorCount = 1,
                    .descriptorType =
                        i >= 3 ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
                    .pImageInfo = &images[i],
                };
            }
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, taa_layout, 0, writes);
            ScratchPush::Invalidate();
            cmd.pushConstants(taa_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(tp), &tp);
            cmd.dispatch(DivCeil(r.extent.width, 8), DivCeil(r.extent.height, 8), 1);
            taa_prev_intr[e] = intr;
        }
        if (scaling) {
            const std::array<vk::ImageMemoryBarrier2, 2> blended{
                Barrier(taa_hist[0][w].image, L::eGeneral, L::eGeneral, S::eComputeShader,
                        A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead),
                Barrier(taa_hist[1][w].image, L::eGeneral, L::eGeneral, S::eComputeShader,
                        A::eShaderStorageWrite, S::eComputeShader, A::eShaderSampledRead),
            };
            cmd.pipelineBarrier2(
                {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = blended.data()});
        }
    }
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, query_pool, q0 + 4);
    }
    // eye_upscale / eye_sharpen: both eyes enlarged and sharpened into `eyes`.
    if (scaling) {
        const std::array<VrEyeUpscale::Source, 2> sources{{
            {taa_on ? taa_hist[0][taa_parity].view : eye_src[0].view, source.crops[0].extent},
            {taa_on ? taa_hist[1][taa_parity].view : eye_src[1].view, source.crops[1].extent},
        }};
        const std::array<VrEyeUpscale::Target, 2> targets{{
            {eyes.image, eyes.view, {0, 0}, out[0]},
            {eyes.image, eyes.view, {static_cast<s32>(out[0].width), 0}, out[1]},
        }};
        const VrEyeUpscale::Vignettes vig{
            .buffer = uniforms.buffer,
            .offset = index * uniform_stride + vignette_offset,
            .range = sizeof(VrVignetteWarp::Block),
        };
        const bool recorded =
            upscale.Record(cmd, sources, L::eGeneral, targets, source.sharpen,
                           vignettes ? &vig : nullptr, source.color, bgra, s.gamma);
        stats.vignettes += recorded && vignettes ? 1u : 0u;
    }
    // The next frame's vignettes can come here (DrawOverlay routes them by this).
    vignettes_possible = scaling && upscale.VignettesReady();
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, query_pool, q0 + 5);
    }

    // Into the swapchain image (the compositor is done with it: xrWaitSwapchainImage).
    const vk::Image target = swapchain_images[image_index];
    const std::array<vk::ImageMemoryBarrier2, 2> to_copy{
        Barrier(eyes.image, L::eGeneral, L::eTransferSrcOptimal, S::eComputeShader,
                A::eShaderStorageWrite, S::eCopy, A::eTransferRead),
        Barrier(target, L::eUndefined, L::eTransferDstOptimal, S::eNone, A::eNone, S::eCopy,
                A::eTransferWrite),
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_copy.data()});
    const vk::ImageCopy region{
        .srcSubresource = ColorLayers,
        .srcOffset = {0, 0, 0},
        .dstSubresource = ColorLayers,
        .dstOffset = {0, 0, 0},
        .extent = {eyes.extent.width, eyes.extent.height, 1},
    };
    cmd.copyImage(eyes.image, L::eTransferSrcOptimal, target, L::eTransferDstOptimal, region);
    if (stamp_writer) {
        for (u32 e = 0; e < 2; ++e) {
            const XrRect2Di rect{{e == 0 ? 0 : static_cast<s32>(out[0].width), 0},
                                 {static_cast<s32>(out[e].width), static_cast<s32>(out[e].height)}};
            stamp_writer(cmd, target, rect, source.stamp);
        }
    }
    // XR_KHR_vulkan_enable: released in COLOR_ATTACHMENT_OPTIMAL.
    const vk::ImageMemoryBarrier2 to_release =
        Barrier(target, L::eTransferDstOptimal, L::eColorAttachmentOptimal, S::eAllTransfer,
                A::eTransferWrite, S::eColorAttachmentOutput, A::eColorAttachmentRead);
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_release});
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, query_pool, q0 + 6);
    }
    if (cmd.end() != vk::Result::eSuccess) {
        Disable("command buffer end");
        return false;
    }
    return true;
}

VrLateWarp::Stats VrLateWarp::TakeStats() {
    // VR_GPU_TIMING=1: every span of the warp (the `VR late warp:` line has the total).
    static const bool spans = [] {
        const char* v = std::getenv("VR_GPU_TIMING");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (spans && stats.gpu_samples > 0) {
        const double n = stats.gpu_samples;
        LOG_INFO(Render_Vulkan,
                 "VR late warp gpu: {} warps, per warp avg {:.3f} ms = classify {:.3f} (per "
                 "classified capture {:.3f}, {}) + reproject {:.3f} + warp {:.3f} + eye TAA {:.3f} "
                 "+ upscale/sharpen {:.3f} + copy to the swapchain {:.3f}",
                 stats.gpu_samples, stats.gpu_ms / n,
                 stats.classify_samples ? stats.classify_ms / n : 0.0,
                 stats.classify_samples ? stats.classify_ms / stats.classify_samples : 0.0,
                 stats.classify_samples, stats.reproject_ms / n, stats.warp_ms / n,
                 stats.taa_samples ? stats.taa_ms / n : 0.0,
                 stats.upscale_samples ? stats.upscale_ms / n : 0.0, stats.copy_ms / n);
    }
    return std::exchange(stats, Stats{});
}

std::string VrLateWarp::Describe() const {
    return fmt::format("swapchain {}x{} {}, {} MB of images", swapchain_extent.width,
                       swapchain_extent.height, vk::to_string(vk_format),
                       memory_bytes / (1024 * 1024));
}

} // namespace Vulkan

#endif // ENABLE_OPENXR
