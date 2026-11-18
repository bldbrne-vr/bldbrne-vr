#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_riposte_meter.h"

#include <chrono>
#include <cmath>
#include <string_view>

#include <vk_mem_alloc.h>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_riposte_meter_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// Mirrors MeterPush in vr_riposte_meter.comp.
struct MeterPush {
    s32 rect[2][4];
    u32 mode[4];
    float ease[4];
};
static_assert(sizeof(MeterPush) == 64);

// The meter eases toward each frame's measure with this time constant; a gap longer than
// RestartNs starts a new burst from its own first measure.
constexpr double EaseS = 0.12;
constexpr s64 RestartNs = 250'000'000;

} // Anonymous namespace

VrRiposteMeter::~VrRiposteMeter() {
    Shutdown();
}

bool VrRiposteMeter::Init(vk::Device dev, VmaAllocator alloc) {
    Shutdown();
    device = dev;
    allocator = alloc;
    const auto fail = [this](std::string_view what) {
        LOG_ERROR(Render_Vulkan, "VR riposte meter unavailable ({}): the burst is not drawn", what);
        Shutdown();
        return false;
    };
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    auto [lr, sl] = device.createDescriptorSetLayout({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    if (lr != vk::Result::eSuccess) {
        return fail("descriptor set layout");
    }
    set_layout = sl;
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(MeterPush),
    };
    auto [pr, pl] = device.createPipelineLayout({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (pr != vk::Result::eSuccess) {
        return fail("pipeline layout");
    }
    layout = pl;
    const vk::ShaderModule module =
        Compile(HostShaders::VR_RIPOSTE_METER_COMP, vk::ShaderStageFlagBits::eCompute, device);
    if (!module) {
        return fail("shader");
    }
    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = layout,
    };
    auto [cr, p] = device.createComputePipeline({}, pipeline_info);
    device.destroyShaderModule(module);
    if (cr != vk::Result::eSuccess) {
        return fail("pipeline");
    }
    pipeline = p;
    SetObjectName(device, pipeline, "vr_riposte_meter.comp");
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer handle{};
    if (vmaCreateBuffer(allocator, &info, &alloc_info, &handle, &allocation, nullptr) !=
        VK_SUCCESS) {
        allocation = {};
        return fail("buffer");
    }
    buffer = vk::Buffer{handle};
    SetObjectName(device, buffer, "VR riposte meter");
    last_ns = 0;
    ready = true;
    return true;
}

void VrRiposteMeter::Shutdown() {
    ready = false;
    if (!device) {
        return;
    }
    if (buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(buffer), allocation);
        buffer = vk::Buffer{};
        allocation = {};
    }
    if (pipeline) {
        device.destroyPipeline(pipeline);
        pipeline = vk::Pipeline{};
    }
    if (layout) {
        device.destroyPipelineLayout(layout);
        layout = vk::PipelineLayout{};
    }
    if (set_layout) {
        device.destroyDescriptorSetLayout(set_layout);
        set_layout = vk::DescriptorSetLayout{};
    }
}

void VrRiposteMeter::Record(vk::CommandBuffer cmd, const std::array<Eye, 2>& eyes,
                            bool srgb_bytes, bool bgra, vk::PipelineStageFlags2 readers) {
    if (!ready) {
        return;
    }
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const s64 gap = now - last_ns;
    last_ns = now;
    MeterPush push{};
    for (u32 e = 0; e < 2; ++e) {
        push.rect[e][0] = eyes[e].rect.offset.x;
        push.rect[e][1] = eyes[e].rect.offset.y;
        push.rect[e][2] = static_cast<s32>(eyes[e].rect.extent.width);
        push.rect[e][3] = static_cast<s32>(eyes[e].rect.extent.height);
    }
    push.mode[0] = srgb_bytes ? 1u : 0u;
    push.mode[1] = bgra ? 1u : 0u;
    push.ease[0] = gap >= RestartNs || gap <= 0
                       ? 1.0f
                       : static_cast<float>(1.0 - std::exp(-static_cast<double>(gap) * 1e-9 /
                                                           EaseS));
    // The last frame's readers are done with the value and its meter's write is visible here.
    const vk::BufferMemoryBarrier2 before{
        .srcStageMask = S::eComputeShader | readers,
        .srcAccessMask = A::eShaderStorageWrite,
        .dstStageMask = S::eComputeShader,
        .dstAccessMask = A::eShaderStorageRead | A::eShaderStorageWrite,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &before});
    const std::array<vk::DescriptorImageInfo, 2> images{{
        {.imageView = eyes[0].view, .imageLayout = eyes[0].layout},
        {.imageView = eyes[1].view, .imageLayout = eyes[1].layout},
    }};
    const vk::DescriptorBufferInfo meter{.buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &images[0]},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &images[1]},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &meter},
    }};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
    ScratchPush::Invalidate();
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmd.dispatch(1, 1, 1);
    const vk::BufferMemoryBarrier2 after{
        .srcStageMask = S::eComputeShader,
        .srcAccessMask = A::eShaderStorageWrite,
        .dstStageMask = readers,
        .dstAccessMask = A::eShaderStorageRead,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    cmd.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &after});
}

} // namespace Vulkan
