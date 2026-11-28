// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_depth_pyramid.h"
#include <algorithm>
#include <array>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "video_core/host_shaders/vr_depth_pyramid_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vram_report.h"
#include "shader_recompiler/ir/passes/light_shadow.h"

namespace Vulkan {
static_assert(VrDepthPyramid::Levels == Shader::Optimization::LightShadow::SceneMaps::PyramidLevels);
static_assert(VrDepthPyramid::HeaderWords == Shader::Optimization::LightShadow::SceneMaps::PyramidHeaderWords);
namespace {
struct Push {
    uint32_t width;
    uint32_t height;
    uint32_t tag;
    uint32_t pad;
};
} // namespace

VrDepthPyramid::VrDepthPyramid(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}

VrDepthPyramid::~VrDepthPyramid() {
    if (buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer, memory);
    }
}

void VrDepthPyramid::Release() {
    if (!buffer) {
        return;
    }
    scheduler.DeferOperation([allocator = instance.GetAllocator(), old = buffer, old_memory = memory] {
        vmaDestroyBuffer(allocator, old, old_memory);
    });
    buffer = vk::Buffer{};
    memory = nullptr;
    capacity = 0;
    used = 0;
}

uint64_t VrDepthPyramid::BytesFor(uint32_t width, uint32_t height) {
    uint64_t texels = 0;
    for (uint32_t level = 1; level <= Levels; ++level) {
        const uint32_t round = (1u << level) - 1u;
        const uint64_t w = std::max((width + round) >> level, 1u);
        const uint64_t h = std::max((height + round) >> level, 1u);
        texels += 2 * w * h; // both eyes
    }
    return (HeaderWords + 2 * texels) * sizeof(uint32_t);
}

bool VrDepthPyramid::Init() {
    if (tried) {
        return ready;
    }
    tried = true;
    const auto device = instance.GetDevice();
    auto smp = device.createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                          .minFilter = vk::Filter::eNearest,
                                          .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                          .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                          .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                          .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                          .maxLod = 0.0f});
    if (smp.result != vk::Result::eSuccess) {
        return false;
    }
    sampler = std::move(smp.value);
    const vk::Sampler immutable = *sampler;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute, &immutable},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute, &immutable},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = static_cast<uint32_t>(bindings.size()),
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess) {
        return false;
    }
    descriptors = std::move(d.value);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                .pSetLayouts = &*descriptors,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push});
    if (l.result != vk::Result::eSuccess) {
        return false;
    }
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_DEPTH_PYRAMID_COMP, vk::ShaderStageFlagBits::eCompute, device);
    if (!module) {
        return false;
    }
    auto p = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
         .layout = *layout});
    device.destroyShaderModule(module);
    if (p.result != vk::Result::eSuccess) {
        return false;
    }
    pipeline = std::move(p.value);
    ready = true;
    LOG_INFO(Render_Vulkan, "VR depth pyramid: ready ({} levels)", Levels);
    return true;
}

bool VrDepthPyramid::Record(const RecorderProxy& cmd, vk::ImageView left, vk::ImageView right,
                            vk::ImageLayout image_layout, uint32_t width, uint32_t height) {
    if (!Init() || !left || width == 0 || height == 0 || width > 8192 || height > 8192) {
        return false;
    }
    const uint64_t bytes = BytesFor(width, height);
    if (bytes > capacity) {
        // Grown (never shrunk): the old buffer freed once the GPU is past this frame.
        if (buffer) {
            scheduler.DeferOperation([allocator = instance.GetAllocator(), old = buffer,
                                      old_memory = memory] {
                vmaDestroyBuffer(allocator, old, old_memory);
            });
            buffer = vk::Buffer{};
            memory = nullptr;
            capacity = 0;
        }
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = bytes,
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer raw{};
        if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &raw, &memory, nullptr) !=
            VK_SUCCESS) {
            memory = nullptr;
            LOG_ERROR(Render_Vulkan, "VR depth pyramid: no buffer ({} KiB)", bytes >> 10);
            return false;
        }
        VideoCore::VramReport::Name(instance.GetAllocator(), memory,
                                    fmt::format("vr depth pyramid ({} KiB)", bytes >> 10));
        buffer = raw;
        capacity = bytes;
        LOG_INFO(Render_Vulkan, "VR depth pyramid: {}x{} a eye, {} KiB", width, height, bytes >> 10);
    }
    used = bytes;
    // The depth's writes (its pass) and the last frame's reads of the buffer before this dispatch.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits2::eLateFragmentTests |
                        vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const std::array<vk::DescriptorImageInfo, 2> images{{
        {vk::Sampler{}, left, image_layout},
        {vk::Sampler{}, right ? right : left, image_layout},
    }};
    const vk::DescriptorBufferInfo out{buffer, 0, bytes};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[0]},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[1]},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &out},
    }};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    const Push push{.width = width, .height = height, .tag = ++counter | 0x40000000u, .pad = 0};
    cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmd.dispatch((width + 31) / 32, (height + 31) / 32, 2);
    // The pyramid before the AO pass reads it.
    const vk::BufferMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &after});
    return true;
}

} // namespace Vulkan
