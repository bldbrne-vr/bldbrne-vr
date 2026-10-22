#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include "video_core/renderer_vulkan/vr_eye_upscale.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_eye_upscale_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_eye_scale.h"

#define A_CPU
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

namespace Vulkan {

namespace {

/// Mirrors UpscalePush in vr_eye_upscale.comp.
struct UpscalePush {
    u32 con0[4];
    u32 con1[4];
    u32 con2[4];
    u32 con3[4];
    s32 dst[4];
    s32 src[4];
    u32 vig[4]; // VR_VIGNETTE: x = the eye
    std::array<float, 4> color;
};
static_assert(sizeof(UpscalePush) == 128);

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

} // Anonymous namespace

VrEyeUpscale::~VrEyeUpscale() {
    Shutdown();
}

vk::Extent2D VrEyeUpscale::Scaled(vk::Extent2D in, float scale) {
    const VrEyeScale::Size out = VrEyeScale::Scaled({in.width, in.height}, scale);
    return {out.width, out.height};
}

bool VrEyeUpscale::Init(vk::Device dev, VmaAllocator alloc) {
    Shutdown();
    device = dev;
    allocator = alloc;
    const auto fail = [this](std::string_view what) {
        LOG_ERROR(Render_Vulkan, "VR eye upscale unavailable ({}): the eyes go as they are", what);
        Shutdown();
        return false;
    };
    // No sampler: the shader fetches texels (a combined immutable sampler read garbage in the
    // game's device, though not in a stand-alone test; texel fetches need none).
    // Binding 2: the vignettes (VR_VIGNETTE variants only; the others never use it).
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
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
        .size = sizeof(UpscalePush),
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
    const auto make = [&](std::vector<std::string> defines, const char* name,
                          std::array<vk::Pipeline, 3>& out) {
        const vk::ShaderModule module = Compile(HostShaders::VR_EYE_UPSCALE_COMP,
                                                vk::ShaderStageFlagBits::eCompute, device,
                                                std::move(defines));
        if (!module) {
            return false;
        }
        const std::array<vk::SpecializationMapEntry, 2> color_entries{{
            {.constantID = 0, .offset = 0, .size = sizeof(u32)},
            {.constantID = 1, .offset = sizeof(u32), .size = sizeof(u32)},
        }};
        std::array<u32, 2> color_flags{};
        const vk::SpecializationInfo color_specialization{
            .mapEntryCount = static_cast<u32>(color_entries.size()),
            .pMapEntries = color_entries.data(),
            .dataSize = sizeof(color_flags),
            .pData = color_flags.data(),
        };
        const vk::ComputePipelineCreateInfo info{
            .stage{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
                .pSpecializationInfo = &color_specialization,
            },
            .layout = layout,
        };
        for (u32 variant = 0; variant < out.size(); ++variant) {
            color_flags = {variant == 2, variant != 0};
            auto [result, p] = device.createComputePipeline({}, info);
            if (result != vk::Result::eSuccess) {
                device.destroyShaderModule(module);
                return false;
            }
            out[variant] = p;
            SetObjectName(device, p, "{} (color {})", name, variant);
        }
        device.destroyShaderModule(module);
        return true;
    };
    if (!make({"VR_EASU"}, "vr_eye_upscale.comp (EASU)", easu) ||
        !make({"VR_RCAS"}, "vr_eye_upscale.comp (RCAS)", rcas)) {
        return fail("pipelines");
    }
    // vignette_in_warp: without them the vignettes stay on the overlay (logged).
    if (!make({"VR_EASU", "VR_VIGNETTE"}, "vr_eye_upscale.comp (EASU + vignettes)", easu_vig) ||
        !make({"VR_RCAS", "VR_VIGNETTE"}, "vr_eye_upscale.comp (RCAS + vignettes)", rcas_vig)) {
        LOG_ERROR(Render_Vulkan, "VR eye upscale: no vignette variants; the vignettes stay on the "
                                 "overlay layer");
        for (vk::Pipeline* p : {&easu_vig[0], &easu_vig[1], &easu_vig[2],
                                &rcas_vig[0], &rcas_vig[1], &rcas_vig[2]}) {
            if (*p) {
                device.destroyPipeline(*p);
                *p = vk::Pipeline{};
            }
        }
    }
    ready = true;
    return true;
}

void VrEyeUpscale::Shutdown() {
    ready = false;
    if (!device) {
        return;
    }
    DestroyMid();
    for (vk::Pipeline* p : {&easu[0], &easu[1], &easu[2], &rcas[0], &rcas[1], &rcas[2],
                            &easu_vig[0], &easu_vig[1], &easu_vig[2],
                            &rcas_vig[0], &rcas_vig[1], &rcas_vig[2]}) {
        if (*p) {
            device.destroyPipeline(*p);
            *p = vk::Pipeline{};
        }
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

void VrEyeUpscale::DestroyImage(Image& image) {
    if (image.view) {
        device.destroyImageView(image.view);
    }
    if (image.image) {
        vmaDestroyImage(allocator, static_cast<VkImage>(image.image), image.allocation);
    }
    image = {};
}

void VrEyeUpscale::DestroyMid() {
    DestroyImage(mid);
    for (auto& r : retired) {
        DestroyImage(r.image);
    }
    retired.clear();
}

bool VrEyeUpscale::EnsureMid(vk::Extent2D extent) {
    if (mid.image && mid.extent.width >= extent.width && mid.extent.height >= extent.height) {
        return true;
    }
    // The previous one may still be read by work in flight: retired, destroyed RetireCalls
    // Prepare calls (frames) later. The sizes only grow with the eye crops, so this is rare (the
    // first frames, a resolution change).
    if (mid.image) {
        retired.push_back({mid, RetireCalls});
        mid = {};
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
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(allocator, &info, &alloc, &handle, &mid.allocation, nullptr) !=
        VK_SUCCESS) {
        mid = {};
        return false;
    }
    mid.image = vk::Image{handle};
    auto [vr, view] = device.createImageView({
        .image = mid.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = ColorRange,
    });
    if (vr != vk::Result::eSuccess) {
        DestroyImage(mid);
        return false;
    }
    mid.view = view;
    mid.extent = extent;
    SetObjectName(device, mid.image, "VR eye upscale {}x{}", extent.width, extent.height);
    return true;
}

bool VrEyeUpscale::Prepare(const std::array<vk::Extent2D, 2>& inputs,
                           const std::array<vk::Extent2D, 2>& outputs, float sharpen) {
    if (!ready) {
        return false;
    }
    for (auto it = retired.begin(); it != retired.end();) {
        if (--it->calls == 0) {
            DestroyImage(it->image);
            it = retired.erase(it);
        } else {
            ++it;
        }
    }
    const bool any_up = outputs[0] != inputs[0] || outputs[1] != inputs[1];
    if (!any_up || !(sharpen > 0.0f)) {
        return true;
    }
    const vk::Extent2D need{outputs[0].width + outputs[1].width,
                            std::max(outputs[0].height, outputs[1].height)};
    if (EnsureMid(need)) {
        return true;
    }
    if (!warned) {
        warned = true;
        LOG_ERROR(Render_Vulkan, "VR eye upscale: no {}x{} image; the eyes go as they are",
                  need.width, need.height);
    }
    return false;
}

bool VrEyeUpscale::Record(vk::CommandBuffer cmd, const std::array<Source, 2>& sources,
                          vk::ImageLayout source_layout, const std::array<Target, 2>& targets,
                          float sharpen, const Vignettes* vignettes,
                          VR::Color::Settings color, bool bgra, float output_gamma) {
    if (!ready) {
        return false;
    }
    if (vignettes != nullptr && (!VignettesReady() || !vignettes->buffer)) {
        vignettes = nullptr;
    }
    sharpen = std::clamp(sharpen, 0.0f, 1.0f);
    output_gamma = output_gamma > 0.0f ? output_gamma : 1.0f;
    const u32 output_variant = color.Active() ? 2u : (output_gamma != 1.0f ? 1u : 0u);
    std::array<bool, 2> up{};
    for (u32 e = 0; e < 2; ++e) {
        const vk::Extent2D in = sources[e].extent;
        const vk::Extent2D out = targets[e].extent;
        if (!sources[e].view || !targets[e].view || in.width == 0 || in.height == 0 ||
            out.width < in.width || out.height < in.height) {
            return false;
        }
        up[e] = out != in;
    }
    const bool any_up = up[0] || up[1];
    // EASU into `mid` when RCAS follows, else straight into the targets.
    const bool use_mid = any_up && sharpen > 0.0f;
    std::array<vk::Offset2D, 2> mid_offset{};
    if (use_mid) {
        mid_offset[1].x = static_cast<s32>(targets[0].extent.width);
        const vk::Extent2D need{targets[0].extent.width + targets[1].extent.width,
                                std::max(targets[0].extent.height, targets[1].extent.height)};
        if (!EnsureMid(need)) {
            if (!warned) {
                warned = true;
                LOG_ERROR(Render_Vulkan, "VR eye upscale: no {}x{} image; the eyes go as they are",
                          need.width, need.height);
            }
            return false;
        }
        // Rewritten whole (the eyes' rects): its earlier contents are not needed, only the
        // earlier reads of it finished.
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = mid.image,
            .subresourceRange = ColorRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
    }
    // `last`: this dispatch writes the eye last (the vignettes go in with it, when there are any).
    const auto bind = [&](vk::ImageView in, vk::ImageLayout in_layout, vk::ImageView out,
                          bool last = false) {
        const std::array<vk::DescriptorImageInfo, 2> images{{
            {.imageView = in, .imageLayout = in_layout},
            {.imageView = out, .imageLayout = vk::ImageLayout::eGeneral},
        }};
        const vk::DescriptorBufferInfo vig_info{
            .buffer = vignettes != nullptr ? vignettes->buffer : vk::Buffer{},
            .offset = vignettes != nullptr ? vignettes->offset : 0,
            .range = vignettes != nullptr ? vignettes->range : 0,
        };
        const std::array<vk::WriteDescriptorSet, 3> writes{{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &images[0]},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &images[1]},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eUniformBuffer,
             .pBufferInfo = &vig_info},
        }};
        const u32 count = last && vignettes != nullptr ? 3u : 2u;
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, count, writes.data());
    };
    if (any_up) {
        // EASU writes the targets itself (no RCAS after it): the vignettes go in with it.
        const bool easu_last = !use_mid && vignettes != nullptr;
        const u32 easu_variant = use_mid ? 0u : output_variant;
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,
                         easu_last ? easu_vig[easu_variant] : easu[easu_variant]);
        for (u32 e = 0; e < 2; ++e) {
            if (!up[e]) {
                continue;
            }
            const vk::Extent2D in = sources[e].extent;
            const vk::Extent2D out = targets[e].extent;
            UpscalePush push{};
            FsrEasuCon(push.con0, push.con1, push.con2, push.con3, static_cast<AF1>(in.width),
                       static_cast<AF1>(in.height), static_cast<AF1>(in.width),
                       static_cast<AF1>(in.height), static_cast<AF1>(out.width),
                       static_cast<AF1>(out.height));
            const vk::Offset2D o = use_mid ? mid_offset[e] : targets[e].offset;
            push.dst[0] = o.x;
            push.dst[1] = o.y;
            push.dst[2] = static_cast<s32>(out.width);
            push.dst[3] = static_cast<s32>(out.height);
            static const bool debug_fill = [] {
                const char* v = std::getenv("VR_UPSCALE_DEBUG");
                return v != nullptr && std::atoi(v) == 1;
            }();
            if (debug_fill) {
                push.src[2] = -1;
            }
            push.vig[0] = e;
            push.vig[1] = std::bit_cast<u32>(use_mid ? 1.0f : output_gamma);
            push.color = use_mid ? VR::Color::Settings{}.Push() : color.Push(bgra);
            bind(sources[e].view, source_layout, use_mid ? mid.view : targets[e].view, easu_last);
            ScratchPush::Invalidate();
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmd.dispatch(DivCeil(out.width, 16), DivCeil(out.height, 16), 1);
            if (!logged) {
                logged = true;
                LOG_INFO(Render_Vulkan,
                         "VR eye upscale: EASU {}x{} -> {}x{} at ({}, {}){}, {} groups x {}, "
                         "con0 {:08x} {:08x} {:08x} {:08x}, sharpen {:.2f}{}",
                         in.width, in.height, out.width, out.height, push.dst[0], push.dst[1],
                         use_mid ? " in the intermediate image" : "", DivCeil(out.width, 16),
                         DivCeil(out.height, 16), push.con0[0], push.con0[1], push.con0[2],
                         push.con0[3], sharpen, debug_fill ? " (VR_UPSCALE_DEBUG=1: fill)" : "");
            }
        }
    }
    // RCAS: the sharpening; also the plain copy of an eye that is not enlarged when there is
    // nothing to sharpen (a zero lobe returns the centre texel exactly).
    const bool rcas_pass = sharpen > 0.0f || !up[0] || !up[1];
    if (!rcas_pass) {
        return true;
    }
    if (use_mid) {
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = mid.image,
            .subresourceRange = ColorRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    }
    // RCAS always writes the target last.
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,
                     vignettes != nullptr ? rcas_vig[output_variant] : rcas[output_variant]);
    for (u32 e = 0; e < 2; ++e) {
        if (up[e] && !use_mid) {
            continue; // EASU wrote the target, nothing to sharpen
        }
        const vk::Extent2D out = targets[e].extent;
        UpscalePush push{};
        // FsrRcasCon's con[0] is the lobe's linear strength (exp2(-stops)).
        push.con0[0] = std::bit_cast<u32>(sharpen);
        push.dst[0] = targets[e].offset.x;
        push.dst[1] = targets[e].offset.y;
        push.dst[2] = static_cast<s32>(out.width);
        push.dst[3] = static_cast<s32>(out.height);
        push.vig[0] = e;
        push.vig[1] = std::bit_cast<u32>(output_gamma);
        push.color = color.Push(bgra);
        if (up[e]) {
            push.src[0] = mid_offset[e].x;
            push.src[1] = mid_offset[e].y;
            bind(mid.view, vk::ImageLayout::eGeneral, targets[e].view, true);
        } else {
            bind(sources[e].view, source_layout, targets[e].view, true);
        }
        push.src[2] = static_cast<s32>(out.width);
        push.src[3] = static_cast<s32>(out.height);
        ScratchPush::Invalidate();
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch(DivCeil(out.width, 16), DivCeil(out.height, 16), 1);
    }
    return true;
}

} // namespace Vulkan
