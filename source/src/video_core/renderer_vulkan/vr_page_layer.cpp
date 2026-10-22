// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include "video_core/renderer_vulkan/vr_page_layer.h"

#ifdef ENABLE_OPENXR

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>
#include <openxr/openxr_platform.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vr_device_lost.h"

namespace Vulkan {

namespace {
constexpr u64 WaitImageTimeoutNs = 1'000'000; // 1 ms: never stall the frame thread
constexpr u64 ShutdownTimeoutNs = 2'000'000'000;
} // namespace

VrPageLayer::~VrPageLayer() {
    Shutdown();
}

bool VrPageLayer::Fail(std::string_view what) {
    if (!failed) {
        LOG_ERROR(Render_Vulkan, "VR page layer disabled: {}", what);
    }
    failed = true;
    return false;
}

bool VrPageLayer::Init(const DeviceContext& device, XrSession xr_session,
                       std::span<const s64> formats, u32 w, u32 h) {
    if (initialized || failed) {
        return initialized && !failed;
    }
    dev = device;
    session = xr_session;
    width = w;
    height = h;
    if (!dev.device || !dev.queue || !dev.allocator || session == XR_NULL_HANDLE || w == 0 ||
        h == 0) {
        return Fail("missing device or session handles");
    }
    static constexpr std::array preferred{static_cast<s64>(VK_FORMAT_R8G8B8A8_SRGB),
                                          static_cast<s64>(VK_FORMAT_B8G8R8A8_SRGB)};
    s64 chosen = 0;
    for (const s64 f : preferred) {
        if (std::ranges::find(formats, f) != formats.end()) {
            chosen = f;
            break;
        }
    }
    if (chosen == 0) {
        return Fail("the runtime offers no RGBA8 / BGRA8 sRGB swapchain format");
    }
    format = static_cast<vk::Format>(chosen);
    bgra = chosen == static_cast<s64>(VK_FORMAT_B8G8R8A8_SRGB);

    // The mip chain (see the header): a level at half size when the format blits linearly.
    constexpr vk::FormatFeatureFlags blit_features =
        vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    const bool blits =
        dev.physical_device &&
        (dev.physical_device.getFormatProperties(format).optimalTilingFeatures & blit_features) ==
            blit_features;
    // DeviceContext::swapchain_mips: false with VR_UI_NO_MIPS, and on every runtime but Meta's
    // unless VR_SWAPCHAIN_MIPS=1 (vr_xr_queue_policy.h SwapchainMips).
    mips = blits && dev.swapchain_mips && std::getenv("VR_UI_NO_MIPS") == nullptr &&
                   height % 2 == 0
               ? 2
               : 1;
    XrResult created = XR_ERROR_RUNTIME_FAILURE;
    for (;;) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags =
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        if (mips > 1) {
            info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; // the mip blit
        }
        info.format = chosen;
        info.sampleCount = 1;
        info.width = width;
        info.height = 2 * height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = mips;
        created = xrCreateSwapchain(session, &info, &swapchain);
        if (XR_SUCCEEDED(created) || mips == 1) {
            break;
        }
        LOG_INFO(Render_Vulkan, "VR page layer: the runtime refused {} mip levels ({}); one level",
                 mips, static_cast<s32>(created));
        swapchain = XR_NULL_HANDLE;
        mips = 1;
    }
    if (XR_FAILED(created)) {
        swapchain = XR_NULL_HANDLE;
        return Fail(fmt::format("xrCreateSwapchain {}x{} failed ({})", width, 2 * height,
                                static_cast<s32>(created)));
    }
    u32 count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    std::vector<XrSwapchainImageVulkanKHR> xr_images(
        count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, {}});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())))) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    for (const auto& image : xr_images) {
        images.emplace_back(image.image);
    }
    image_initialized.assign(images.size(), false);

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = static_cast<VkDeviceSize>(width) * height * 2 * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc{};
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkBuffer buffer{};
    VmaAllocationInfo alloc_info{};
    if (const VkResult r = vmaCreateBuffer(dev.allocator, &buffer_info, &alloc, &buffer,
                                           &staging_allocation, &alloc_info);
        r != VK_SUCCESS) {
        return Fail(fmt::format("staging buffer ({})", static_cast<s32>(r)));
    }
    staging = vk::Buffer{buffer};
    staging_mapped = alloc_info.pMappedData;
    if (staging_mapped == nullptr) {
        return Fail("staging buffer not mapped");
    }
    auto [pool_result, new_pool] = dev.device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = dev.queue_family,
    });
    if (pool_result != vk::Result::eSuccess) {
        return Fail("command pool");
    }
    pool = new_pool;
    auto [cmd_result, cmds] = dev.device.allocateCommandBuffers({
        .commandPool = pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    if (cmd_result != vk::Result::eSuccess || cmds.empty()) {
        return Fail("command buffer");
    }
    cmd = cmds[0];
    auto [fence_result, new_fence] = dev.device.createFence({});
    if (fence_result != vk::Result::eSuccess) {
        return Fail("fence");
    }
    fence = new_fence;
    initialized = true;
    LOG_INFO(Render_Vulkan,
             "VR page layer: swapchain {}x{} ({} images, {}, {} mip level{}), {} MB staging",
             width, 2 * height, images.size(), vk::to_string(format), mips, mips > 1 ? "s" : "",
             (static_cast<u64>(width) * height * 8) >> 20);
    return true;
}

void VrPageLayer::Shutdown() {
    if (dev.device) {
        if (fence && pending) {
            (void)dev.device.waitForFences(fence, VK_TRUE, ShutdownTimeoutNs);
        }
        if (fence) {
            dev.device.destroyFence(fence);
        }
        if (pool) {
            dev.device.destroyCommandPool(pool);
        }
        if (staging) {
            vmaDestroyBuffer(dev.allocator, static_cast<VkBuffer>(staging), staging_allocation);
        }
    }
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
    }
    fence = vk::Fence{};
    pool = vk::CommandPool{};
    cmd = vk::CommandBuffer{};
    staging = vk::Buffer{};
    staging_allocation = {};
    staging_mapped = nullptr;
    swapchain = XR_NULL_HANDLE;
    images.clear();
    image_initialized.clear();
    initialized = false;
    has_image = acquired = waited = pending = staged = false;
}

bool VrPageLayer::Stage(const u8* page_rgba, const u8* glow_rgba) {
    if (!initialized || failed) {
        return false;
    }
    if (pending) {
        const vk::Result status = dev.device.getFenceStatus(fence);
        if (status == vk::Result::eNotReady) {
            return false; // the last copy is still on the GPU: next frame
        }
        if (status != vk::Result::eSuccess) {
            VrDeviceLost::Note(status, "the VR page's fence");
            return Fail(fmt::format("fence status {}", vk::to_string(status)));
        }
        pending = false;
    }
    // The pixels (BGRA swizzled for a BGRA swapchain).
    const size_t plane = static_cast<size_t>(width) * height * 4;
    u8* const dst = static_cast<u8*>(staging_mapped);
    for (int part = 0; part < 2; ++part) {
        const u8* src = part == 0 ? page_rgba : glow_rgba;
        u8* out = dst + part * plane;
        if (!bgra) {
            std::memcpy(out, src, plane);
        } else {
            for (size_t i = 0; i < plane; i += 4) {
                out[i + 0] = src[i + 2];
                out[i + 1] = src[i + 1];
                out[i + 2] = src[i + 0];
                out[i + 3] = src[i + 3];
            }
        }
    }
    vmaFlushAllocation(dev.allocator, staging_allocation, 0, VK_WHOLE_SIZE);
    staged = true;
    return true;
}

bool VrPageLayer::Upload() {
    if (!initialized || failed || !staged) {
        return false;
    }
    if (!acquired) {
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (const XrResult r = xrAcquireSwapchainImage(swapchain, &acquire, &image_index);
            XR_FAILED(r)) {
            return Fail(fmt::format("xrAcquireSwapchainImage failed ({})", static_cast<s32>(r)));
        }
        acquired = true;
        waited = false;
    }
    if (!waited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = WaitImageTimeoutNs;
        const XrResult r = xrWaitSwapchainImage(swapchain, &wait);
        if (r == XR_TIMEOUT_EXPIRED) {
            return false; // still acquired: waited for again next frame
        }
        if (XR_FAILED(r)) {
            return Fail(fmt::format("xrWaitSwapchainImage failed ({})", static_cast<s32>(r)));
        }
        waited = true;
    }

    const vk::Image image = images[image_index];
    if (cmd.reset() != vk::Result::eSuccess ||
        cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) !=
            vk::Result::eSuccess) {
        return Fail("command buffer begin");
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, mips, 0, 1};
    const vk::ImageMemoryBarrier to_dst{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined, // fully overwritten
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits::eTopOfPipe,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_dst);
    const vk::BufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, 2 * height, 1},
    };
    cmd.copyBufferToImage(staging, image, vk::ImageLayout::eTransferDstOptimal, copy);
    const auto level = [](u32 mip) {
        return vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1};
    };
    // Every level back to colour-attachment layout (OpenXR's at release); level 0 comes from
    // the transfer source layout after the blit.
    std::array<vk::ImageMemoryBarrier, 2> to_color{};
    for (u32 mip = 0; mip < 2; ++mip) {
        to_color[mip] = vk::ImageMemoryBarrier{
            .srcAccessMask = mip == 0 && mips > 1 ? vk::AccessFlagBits::eTransferRead
                                                  : vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                             vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = mip == 0 && mips > 1 ? vk::ImageLayout::eTransferSrcOptimal
                                              : vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = level(mip),
        };
    }
    if (mips > 1) {
        // Level 1 from level 0 (linear, in linear light on the sRGB format): both halves at
        // half size, the page's rows 0..H-1 into 0..H/2-1 and the glow's below them.
        const vk::ImageMemoryBarrier to_src{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = level(0),
        };
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_src);
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{static_cast<s32>(width),
                                                  static_cast<s32>(2 * height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 1, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{static_cast<s32>(std::max(width / 2, 1u)),
                                                  static_cast<s32>(height), 1}},
        };
        cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    }
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                        vk::ArrayProxy<const vk::ImageMemoryBarrier>(mips, to_color.data()));
    if (cmd.end() != vk::Result::eSuccess) {
        return Fail("command buffer end");
    }
    if (dev.device.resetFences(fence) != vk::Result::eSuccess) {
        return Fail("fence reset");
    }
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    if (const vk::Result r = dev.queue.submit(submit, fence); r != vk::Result::eSuccess) {
        VrDeviceLost::Note(r, "the VR page's submit");
        return Fail(fmt::format("queue submit: {}", vk::to_string(r)));
    }
    pending = true;
    image_initialized[image_index] = true;
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (const XrResult r = xrReleaseSwapchainImage(swapchain, &release); XR_FAILED(r)) {
        return Fail(fmt::format("xrReleaseSwapchainImage failed ({})", static_cast<s32>(r)));
    }
    acquired = waited = false;
    staged = false;
    has_image = true;
    ++uploads;
    return true;
}

} // namespace Vulkan

#endif // ENABLE_OPENXR
