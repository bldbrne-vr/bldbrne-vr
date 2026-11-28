// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_shadow_edges.h"
#include <algorithm>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "shader_recompiler/ir/passes/light_shadow.h"
#include "video_core/host_shaders/vr_shadow_edges_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vram_report.h"

namespace Vulkan {
static_assert(VrShadowEdges::ParamWords == Shader::Optimization::LightShadow::SceneMaps::EdgeParamWords);
static_assert(VrShadowEdges::DataAt == Shader::Optimization::LightShadow::SceneMaps::EdgeDataAt);

VrShadowEdges::VrShadowEdges(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}

VrShadowEdges::~VrShadowEdges() {
    if (buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer, memory);
    }
}

void VrShadowEdges::Release() {
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

bool VrShadowEdges::Init() {
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
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute, &immutable},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute, &immutable},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = static_cast<uint32_t>(bindings.size()),
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess) {
        return false;
    }
    descriptors = std::move(d.value);
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*descriptors});
    if (l.result != vk::Result::eSuccess) {
        return false;
    }
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_SHADOW_EDGES_COMP, vk::ShaderStageFlagBits::eCompute, device);
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
    LOG_INFO(Render_Vulkan, "VR shadow edges: ready");
    return true;
}

void VrShadowEdges::FillHead(std::span<uint32_t, DataAt> head, std::span<const uint32_t, ParamWords> params,
                             uint32_t serial, uint32_t width, uint32_t height, uint32_t bits) {
    std::copy(params.begin(), params.end(), head.begin());
    head[ParamWords + 0] = serial;
    head[ParamWords + 1] = width;
    head[ParamWords + 2] = height;
    head[ParamWords + 3] = WordsARow(width, bits);
}

bool VrShadowEdges::Record(const RecorderProxy& cmd, vk::ImageView left, vk::ImageView right,
                           vk::ImageLayout image_layout, uint32_t width, uint32_t height,
                           vk::Buffer mask, uint64_t mask_bytes, vk::Buffer head, uint64_t head_offset,
                           uint32_t bits) {
    if (!Init() || !left || !mask || !head || mask_bytes == 0 || width == 0 || height == 0 ||
        width > 8192 || height > 8192) {
        return false;
    }
    const uint64_t bytes = BytesFor(width, height, bits);
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
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer raw{};
        if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &raw, &memory, nullptr) !=
            VK_SUCCESS) {
            memory = nullptr;
            LOG_ERROR(Render_Vulkan, "VR shadow edges: no buffer ({} KiB)", bytes >> 10);
            return false;
        }
        VideoCore::VramReport::Name(instance.GetAllocator(), memory,
                                    fmt::format("vr shadow edges ({} KiB)", bytes >> 10));
        buffer = raw;
        capacity = bytes;
        LOG_INFO(Render_Vulkan, "VR shadow edges: {}x{} a eye, {} KiB", width, height, bytes >> 10);
    }
    used = bytes;
    // The parameters and the header (FillHead's, in the stream buffer), before the depth's and the
    // mask's writers are waited for.
    // (Last frame's lamp draws and pass done with the buffer before it is written again.)
    const vk::MemoryBarrier2 before_update{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before_update});
    // (A recorded copy: the recorder keeps queueing. updateBuffer had no packet, and its raw
    // command buffer drained the recorder once a frame.)
    cmd.copyBuffer(head, buffer, vk::BufferCopy{head_offset, 0, DataAt * sizeof(uint32_t)});
    // The depth's writes (its pass), the mask's (the AO pass) and the update before the dispatch.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits2::eLateFragmentTests |
                        vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const std::array<vk::DescriptorImageInfo, 2> images{{
        {vk::Sampler{}, left, image_layout},
        {vk::Sampler{}, right ? right : left, image_layout},
    }};
    const vk::DescriptorBufferInfo mask_info{mask, 0, mask_bytes};
    const vk::DescriptorBufferInfo out{buffer, 0, bytes};
    const std::array<vk::WriteDescriptorSet, 4> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[0]},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[1]},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &mask_info},
        {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &out},
    }};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    const uint32_t wq = WordsARow(width, bits);
    cmd.dispatch((wq + 7) / 8, (height + 7) / 8, 2);
    // The bits (and the header) before the lamp draws read them.
    const vk::BufferMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
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
