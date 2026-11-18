// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {
// Immutable 128px sRGB iris, shared by both eyes of its owning renderer. No file I/O,
// frame allocations or CPU mip generation. Owners must drain GPU work before destruction.
// Keep the 85 KiB upload buffer alive with the image: no guessed retirement frame count.
class VrKosIris {
public:
    VrKosIris() = default;
    ~VrKosIris() { Shutdown(); }
    VrKosIris(const VrKosIris&) = delete;
    VrKosIris& operator=(const VrKosIris&) = delete;
    bool Init(vk::Device device, VmaAllocator allocator);
    void Shutdown();
    bool Ready() const { return image && view && sampler && staging; }
    // First call records the upload and transfer->shader visibility barrier. Call outside
    // rendering on the same ordered queue that will sample this instance thereafter.
    void Upload(vk::CommandBuffer cmd, vk::PipelineStageFlags2 readers);
    vk::DescriptorImageInfo Descriptor() const {
        return {.sampler = sampler, .imageView = view,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    }
private:
    vk::Device device;
    VmaAllocator allocator{};
    vk::Image image;
    VmaAllocation image_allocation{};
    vk::ImageView view;
    vk::Sampler sampler;
    vk::Buffer staging;
    VmaAllocation staging_allocation{};
    bool uploaded{false};
};
} // namespace Vulkan
