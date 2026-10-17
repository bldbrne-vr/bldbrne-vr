// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include "common/logging/log.h"
#include "video_core/host_shaders/vr_temporal_motion_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_camera_motion.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
namespace Vulkan {
VrCameraMotion::VrCameraMotion(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
VrCameraMotion::~VrCameraMotion() {
    if (layout || pipeline || descriptors)
        scheduler.DeferOperation(
            [d = std::move(descriptors), l = std::move(layout), p = std::move(pipeline)]() mutable {
                p.reset();
                l.reset();
                d.reset();
            });
}
bool VrCameraMotion::Init() {
    if (!instance.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats)
        return false;
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
        {{0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
         {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
         {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute}}};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = 3,
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess)
        return false;
    descriptors = std::move(d.value);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(VrMotionCamera::Constants)};
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                .pSetLayouts = &*descriptors,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push});
    if (l.result != vk::Result::eSuccess)
        return false;
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_TEMPORAL_MOTION_COMP, vk::ShaderStageFlagBits::eCompute, device);
    if (!module)
        return false;
    auto p = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
         .layout = *layout});
    device.destroyShaderModule(module);
    if (p.result != vk::Result::eSuccess)
        return false;
    pipeline = std::move(p.value);
    return true;
}
bool VrCameraMotion::Record(const VR::Graphics::Config& accepted, bool provider_ready,
                            const std::array<Eye, 2>& eyes, const Eye* twin) {
    if (accepted.upscaler == VR::Graphics::Upscaler::Off || !provider_ready || failed)
        return false;
    const std::array<const Eye*, 3> passes{&eyes[0], &eyes[1], twin};
    const uint32_t count = twin ? 3u : 2u;
    for (uint32_t pass = 0; pass < count; ++pass) {
        const auto& eye = *passes[pass];
        if (!eye.depth || !eye.motion_view || !eye.motion ||
            (eye.camera.mode == 1 && !eye.object) || eye.camera.mode > 1)
            return false;
        for (float n : eye.camera.size)
            if (!std::isfinite(n) || n < 1 || n > 16384 || n != std::floor(n))
                return false;
        for (float n : eye.camera.reproject)
            if (!std::isfinite(n))
                return false;
        for (float n : eye.camera.projection)
            if (!std::isfinite(n))
                return false;
        for (float n : eye.camera.previous_projection)
            if (!std::isfinite(n))
                return false;
        for (float n : eye.camera.offsets)
            if (!std::isfinite(n))
                return false;
        for (float n : eye.camera.jitter)
            if (!std::isfinite(n))
                return false;
        for (float n : eye.camera.previous_jitter)
            if (!std::isfinite(n))
                return false;
        if (std::abs(eye.camera.projection[0]) < 1e-6f ||
            std::abs(eye.camera.projection[1]) < 1e-6f)
            return false;
    }
    if (eyes[0].motion == eyes[1].motion ||
        (twin && (twin->motion == eyes[0].motion || twin->motion == eyes[1].motion)))
        return false; // Own single-layer output per eye (and for the twin).
    if (!pipeline && !Init()) {
        failed = true;
        LOG_ERROR(Render_Vulkan, "VR temporal camera motion: pipeline initialization failed");
        return false;
    }
    scheduler.EndRendering();
    // Bit 37: recorded through the scheduler like bbport's motion pass; a raw borrow here
    // drained the recorder and kept the rest of the command buffer on this thread.
    const auto record = [&](auto cmd) {
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    ScratchPush::Invalidate();
    std::array<vk::ImageMemoryBarrier2, 3> barriers{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& eye = *passes[i];
        const std::array<vk::DescriptorImageInfo, 3> images{
            {{.imageView = eye.depth, .imageLayout = vk::ImageLayout::eGeneral},
             {.imageView = eye.motion_view, .imageLayout = vk::ImageLayout::eGeneral},
             {.imageView = eye.object ? eye.object : eye.depth,
              .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (uint32_t binding = 0; binding < 3; ++binding)
            writes[binding] = {.dstBinding = binding,
                               .descriptorCount = 1,
                               .descriptorType = binding == 1 ? vk::DescriptorType::eStorageImage
                                                              : vk::DescriptorType::eSampledImage,
                               .pImageInfo = &images[binding]};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0,
                          uint32_t(sizeof(eye.camera)), &eye.camera);
        cmd.dispatch((uint32_t(eye.camera.size[0]) + 7) / 8, (uint32_t(eye.camera.size[1]) + 7) / 8,
                     1);
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                       .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                       .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                       .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                       .oldLayout = vk::ImageLayout::eGeneral,
                       .newLayout = vk::ImageLayout::eGeneral,
                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .image = eye.motion,
                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = count, .pImageMemoryBarriers = barriers.data()});
    };
    if (UpscalerFastOn(UpscalerFastPart::Recording)) {
        record(scheduler.Commands());
        UpscalerFastNote(g_upscaler_fast.raw_avoided);
    } else {
        record(scheduler.CommandBuffer());
    }
    return true;
}
} // namespace Vulkan
