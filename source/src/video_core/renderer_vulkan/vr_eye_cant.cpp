#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_eye_cant.h"

#ifdef ENABLE_OPENXR

#include <cmath>
#include <string_view>

#include <vk_mem_alloc.h>

#include "common/logging/log.h"
#include "core/vr/eye_cant.h"
#include "video_core/host_shaders/vr_eye_cant_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// Mirrors CantPush in vr_eye_cant.comp (push constants).
struct CantPush {
    float m[3][4];    // rows of the eye -> head turn (xyz)
    float dst_tan[4]; // the turned eye's FOV tangents: left, right, up, down
    float src_tan[4]; // the parallel eye's
    s32 dst[4];       // this eye in the turned image: x, y, w, h
    s32 src[4];       // ... in the parallel image
};
static_assert(sizeof(CantPush) == 112);

// An XrFovf as the eye_cant.h helpers take it.
struct CantFov {
    float angle_left, angle_right, angle_up, angle_down;
};
CantFov FromXr(const XrFovf& f) {
    return {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
}

void Tangents(const XrFovf& f, float (&t)[4]) {
    t[0] = std::tan(f.angleLeft);
    t[1] = std::tan(f.angleRight);
    t[2] = std::tan(f.angleUp);
    t[3] = std::tan(f.angleDown);
}

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

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    return {
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

} // Anonymous namespace

VrEyeCant::~VrEyeCant() {
    Shutdown();
}

bool VrEyeCant::Init(vk::Device dev, VmaAllocator alloc) {
    Shutdown();
    device = dev;
    allocator = alloc;
    const auto fail = [this](std::string_view what) {
        LOG_ERROR(Render_Vulkan,
                  "VR canted eyes unavailable ({}): a canted display gets the parallel eyes", what);
        Shutdown();
        return false;
    };
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
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
        .size = sizeof(CantPush),
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
        Compile(HostShaders::VR_EYE_CANT_COMP, vk::ShaderStageFlagBits::eCompute, device, {});
    if (!module) {
        return fail("shader");
    }
    const vk::ComputePipelineCreateInfo info{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = layout,
    };
    auto [result, p] = device.createComputePipeline({}, info);
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        return fail("pipeline");
    }
    pipeline = p;
    SetObjectName(device, pipeline, std::string_view{"vr_eye_cant.comp"});
    return true;
}

void VrEyeCant::Shutdown() {
    if (!device) {
        return;
    }
    Collect(true);
    for (auto& eye : work) {
        for (Image& image : eye) {
            Destroy(image);
        }
    }
    if (pipeline) {
        device.destroyPipeline(pipeline);
    }
    if (layout) {
        device.destroyPipelineLayout(layout);
    }
    if (set_layout) {
        device.destroyDescriptorSetLayout(set_layout);
    }
    pipeline = vk::Pipeline{};
    layout = vk::PipelineLayout{};
    set_layout = vk::DescriptorSetLayout{};
    device = vk::Device{};
    allocator = nullptr;
}

vk::Extent2D VrEyeCant::Extent(const XrFovf& eye, const XrFovf& cover, vk::Extent2D cover_px) {
    const auto px =
        VR::Cant::Extent(FromXr(eye), FromXr(cover), {cover_px.width, cover_px.height});
    return {px[0], px[1]};
}

void VrEyeCant::Record(vk::CommandBuffer cmd, const Eye& eye) const {
    if (!pipeline) {
        return;
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    const std::array<vk::DescriptorImageInfo, 2> images{{
        {.imageView = eye.src, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = eye.dst, .imageLayout = vk::ImageLayout::eGeneral},
    }};
    std::array<vk::WriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < 2; ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &images[i],
        };
    }
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
    CantPush cp{};
    const auto m = VR::Cant::Matrix(eye.cant);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cp.m[i][j] = m[3 * i + j];
        }
    }
    Tangents(eye.dst_fov, cp.dst_tan);
    Tangents(eye.src_fov, cp.src_tan);
    cp.dst[0] = eye.dst_rect.offset.x;
    cp.dst[1] = eye.dst_rect.offset.y;
    cp.dst[2] = static_cast<s32>(eye.dst_rect.extent.width);
    cp.dst[3] = static_cast<s32>(eye.dst_rect.extent.height);
    cp.src[0] = eye.src_rect.offset.x;
    cp.src[1] = eye.src_rect.offset.y;
    cp.src[2] = static_cast<s32>(eye.src_rect.extent.width);
    cp.src[3] = static_cast<s32>(eye.src_rect.extent.height);
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(cp), &cp);
    ScratchPush::Invalidate();
    cmd.dispatch((eye.dst_rect.extent.width + 7) / 8, (eye.dst_rect.extent.height + 7) / 8, 1);
}

bool VrEyeCant::TurnCopy(vk::CommandBuffer cmd, u32 eye, vk::Image src, vk::ImageView src_view,
                         vk::Extent2D src_extent, const XrFovf& src_fov, vk::Image dst,
                         vk::Extent2D dst_extent, const XrFovf& dst_fov,
                         const XrQuaternionf& cant) {
    if (!pipeline || eye > 1 || !src || !dst) {
        return false;
    }
    ++calls;
    Collect(false);
    Image& parallel = work[eye][0];
    Image& turned = work[eye][1];
    if ((!src_view && !EnsureImage(parallel, src_extent)) || !EnsureImage(turned, dst_extent)) {
        return false;
    }
    using L = vk::ImageLayout;
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    // `turned` again (the last frame's copy out of it, and everything before, is ordered).
    const vk::ImageMemoryBarrier2 to_write =
        Barrier(turned.image, L::eUndefined, L::eGeneral, S::eAllCommands,
                A::eMemoryRead | A::eMemoryWrite, S::eComputeShader, A::eShaderStorageWrite);
    if (src_view) {
        // In place: the eye from transfer reads to storage reads, and back after the turn.
        const std::array<vk::ImageMemoryBarrier2, 2> to_turn{
            to_write,
            Barrier(src, L::eTransferSrcOptimal, L::eGeneral, S::eAllCommands, A::eMemoryWrite,
                    S::eComputeShader, A::eShaderStorageRead),
        };
        cmd.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_turn.data()});
    } else {
        // Its bytes into `parallel` first (any 32-bit colour format; the last frame's turn may
        // still read it).
        const std::array<vk::ImageMemoryBarrier2, 2> to_copy{
            Barrier(parallel.image, L::eUndefined, L::eTransferDstOptimal, S::eAllCommands,
                    A::eMemoryRead | A::eMemoryWrite, S::eCopy, A::eTransferWrite),
            to_write,
        };
        cmd.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_copy.data()});
        cmd.copyImage(src, L::eTransferSrcOptimal, parallel.image, L::eTransferDstOptimal,
                      vk::ImageCopy{
                          .srcSubresource = ColorLayers,
                          .dstSubresource = ColorLayers,
                          .extent = {src_extent.width, src_extent.height, 1},
                      });
        const vk::ImageMemoryBarrier2 to_turn =
            Barrier(parallel.image, L::eTransferDstOptimal, L::eGeneral, S::eCopy,
                    A::eTransferWrite, S::eComputeShader, A::eShaderStorageRead);
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_turn});
    }
    Record(cmd, {src_view ? src_view : parallel.view, {{0, 0}, src_extent}, src_fov, turned.view,
                 {{0, 0}, dst_extent}, dst_fov, cant});
    std::array<vk::ImageMemoryBarrier2, 2> to_out{
        Barrier(turned.image, L::eGeneral, L::eTransferSrcOptimal, S::eComputeShader,
                A::eShaderStorageWrite, S::eCopy, A::eTransferRead),
        Barrier(src, L::eGeneral, L::eTransferSrcOptimal, S::eComputeShader,
                A::eShaderStorageRead, S::eAllCommands, A::eMemoryRead),
    };
    cmd.pipelineBarrier2(
        {.imageMemoryBarrierCount = src_view ? 2u : 1u, .pImageMemoryBarriers = to_out.data()});
    cmd.copyImage(turned.image, L::eTransferSrcOptimal, dst, L::eTransferDstOptimal,
                  vk::ImageCopy{
                      .srcSubresource = ColorLayers,
                      .dstSubresource = ColorLayers,
                      .extent = {dst_extent.width, dst_extent.height, 1},
                  });
    return true;
}

bool VrEyeCant::EnsureImage(Image& image, vk::Extent2D extent) {
    if (image.image && image.extent == extent) {
        return true;
    }
    if (image.image) {
        retired.push_back({image, calls});
        image = {};
    }
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(allocator, &info, &alloc, &handle, &image.allocation, nullptr) !=
        VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "VR canted eyes: a {}x{} working image failed", extent.width,
                  extent.height);
        image = {};
        return false;
    }
    image.image = vk::Image{handle};
    image.extent = extent;
    auto [view_result, view] = device.createImageView({
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = ColorRange,
    });
    if (view_result != vk::Result::eSuccess) {
        Destroy(image);
        return false;
    }
    image.view = view;
    SetObjectName(device, image.image, std::string_view{"VR canted eye"});
    return true;
}

void VrEyeCant::Destroy(Image& image) {
    if (image.view) {
        device.destroyImageView(image.view);
    }
    if (image.image) {
        vmaDestroyImage(allocator, static_cast<VkImage>(image.image), image.allocation);
    }
    image = {};
}

void VrEyeCant::Collect(bool all) {
    std::erase_if(retired, [&](Retired& r) {
        if (!all && calls - r.at < RetireCalls) {
            return false;
        }
        Destroy(r.image);
        return true;
    });
}

} // namespace Vulkan

#endif // ENABLE_OPENXR
