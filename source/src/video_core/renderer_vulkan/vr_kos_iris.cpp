// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_kos_iris.h"
#include <cstring>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vr_kos_iris_data.h"

namespace Vulkan {
bool VrKosIris::Init(vk::Device dev, VmaAllocator alloc) {
    Shutdown();
    device = dev;
    allocator = alloc;
    const auto fail = [this]() {
        LOG_WARNING(Render_Vulkan, "VR Kos iris texture unavailable; using procedural eyes");
        Shutdown();
        return false;
    };
    if (!device || !allocator) return fail();
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {KosIrisData::Size, KosIrisData::Size, 1},
        .mipLevels = KosIrisData::Levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw_image{};
    if (vmaCreateImage(allocator, &image_info, &image_alloc, &raw_image,
                       &image_allocation, nullptr) != VK_SUCCESS) return fail();
    image = vk::Image{raw_image};
    auto [vr, v] = device.createImageView({
        .image = image, .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Srgb,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, KosIrisData::Levels, 0, 1},
    });
    if (vr != vk::Result::eSuccess) return fail();
    view = v;
    auto [sr, s] = device.createSampler({
        .magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = float(KosIrisData::Levels - 1),
    });
    if (sr != vk::Result::eSuccess) return fail();
    sampler = s;
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = KosIrisData::Pixels.size(), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo buffer_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo mapped{};
    VkBuffer raw_buffer{};
    if (vmaCreateBuffer(allocator, &buffer_info, &buffer_alloc, &raw_buffer,
                        &staging_allocation, &mapped) != VK_SUCCESS) return fail();
    staging = vk::Buffer{raw_buffer};
    if (!mapped.pMappedData) return fail();
    std::memcpy(mapped.pMappedData, KosIrisData::Pixels.data(), KosIrisData::Pixels.size());
    if (vmaFlushAllocation(allocator, staging_allocation, 0, KosIrisData::Pixels.size()) != VK_SUCCESS)
        return fail();
    return true;
}

void VrKosIris::Upload(vk::CommandBuffer cmd, vk::PipelineStageFlags2 readers) {
    if (uploaded || !Ready()) return;
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, KosIrisData::Levels, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    std::array<vk::BufferImageCopy, KosIrisData::Levels> copies{};
    for (std::uint32_t level = 0; level < copies.size(); ++level) {
        const auto extent = KosIrisData::Size >> level;
        copies[level] = {.bufferOffset = KosIrisData::Offsets[level],
                         .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, 1},
                         .imageExtent = {extent, extent, 1}};
    }
    cmd.copyBufferToImage(staging, image, vk::ImageLayout::eTransferDstOptimal, copies);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = readers;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    uploaded = true;
}

void VrKosIris::Shutdown() {
    if (device) {
        if (sampler) device.destroySampler(sampler);
        if (view) device.destroyImageView(view);
        if (image) vmaDestroyImage(allocator, static_cast<VkImage>(image), image_allocation);
        if (staging) vmaDestroyBuffer(allocator, static_cast<VkBuffer>(staging), staging_allocation);
    }
    sampler = vk::Sampler{};
    view = vk::ImageView{};
    image = vk::Image{};
    staging = vk::Buffer{};
    image_allocation = {};
    staging_allocation = {};
    allocator = {};
    device = vk::Device{};
    uploaded = false;
}
} // namespace Vulkan
