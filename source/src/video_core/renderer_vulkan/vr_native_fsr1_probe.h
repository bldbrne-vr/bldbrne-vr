// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "video_core/renderer_vulkan/vr_eye_upscale.h"
#include "video_core/texture_cache/image.h"
#include <map>
namespace Vulkan {
// Opt-in native FSR1 experiment. Slots are reused only with their owning free Frame.
// Normal presentation never creates this object. All work stays on the draw queue.
class VrNativeFsr1Probe {
    struct Slot {
        std::array<VideoCore::UniqueImage, 2> image;
        std::array<vk::UniqueImageView, 2> view, input_view;
        std::array<vk::Image, 2> input_image{};
    };
    vk::Device device;
    VmaAllocator allocator;
    VrEyeUpscale upscale;
    std::map<u32, Slot> slots;

  public:
    VrNativeFsr1Probe(vk::Device d, VmaAllocator a) : device(d), allocator(a) {
        upscale.Init(d, a);
    }
    // Called only after the owning Frame is free, before destroying its input image.
    // Image handles can be recycled; equality alone cannot identify a new allocation.
    void ReleaseInput(u32 id, u32 eye) {
        const auto found = slots.find(id);
        if (found == slots.end()) return;
        found->second.input_view[eye].reset();
        found->second.input_image[eye] = nullptr;
    }
    bool Record(vk::CommandBuffer cmd, u32 id, const std::array<vk::Image, 2> &input,
                const std::array<vk::Extent2D, 2> &in, const std::array<vk::Extent2D, 2> &out,
                float sharpen, bool bgra, std::array<vk::Image, 2> &result) {
        if (!upscale.Ready() || !upscale.Prepare(in, out, sharpen))
            return false;
        auto &slot = slots[id];
        for (u32 e = 0; e < 2; ++e) {
            if (slot.input_image[e] != input[e]) {
                // EASU/RCAS preserve stored channel order, including BGRA bytes.
                // A BGRA sampling view would swap channels before the raw XR copy.
                auto v = device.createImageViewUnique(
                    {.image = input[e],
                     .viewType = vk::ImageViewType::e2D,
                     .format = vk::Format::eR8G8B8A8Unorm,
                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
                if (v.result != vk::Result::eSuccess)
                    return false;
                slot.input_view[e] = std::move(v.value);
                slot.input_image[e] = input[e];
            }
            auto &target = slot.image[e];
            if (!target.image || target.image_ci.extent.width != out[e].width ||
                target.image_ci.extent.height != out[e].height) {
                slot.view[e].reset();
                target = VideoCore::UniqueImage(device, allocator);
                target.Create({.imageType = vk::ImageType::e2D,
                               .format = vk::Format::eR8G8B8A8Unorm,
                               .extent = {out[e].width, out[e].height, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = vk::SampleCountFlagBits::e1,
                               .tiling = vk::ImageTiling::eOptimal,
                               .usage = vk::ImageUsageFlagBits::eStorage |
                                        vk::ImageUsageFlagBits::eTransferSrc,
                               .initialLayout = vk::ImageLayout::eUndefined});
                auto view = device.createImageViewUnique(
                    {.image = target.image,
                     .viewType = vk::ImageViewType::e2D,
                     .format = vk::Format::eR8G8B8A8Unorm,
                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
                if (view.result != vk::Result::eSuccess)
                    return false;
                slot.view[e] = std::move(view.value);
            }
        }
        using S = vk::PipelineStageFlagBits2;
        using A = vk::AccessFlagBits2;
        using L = vk::ImageLayout;
        std::array<vk::ImageMemoryBarrier2, 4> b{};
        for (u32 e = 0; e < 2; ++e) {
            b[e] = {.srcStageMask = S::eAllCommands,
                    .srcAccessMask = A::eMemoryWrite | A::eMemoryRead,
                    .dstStageMask = S::eComputeShader,
                    .dstAccessMask = A::eShaderSampledRead,
                    .oldLayout = L::eTransferSrcOptimal,
                    .newLayout = L::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = input[e],
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            b[e + 2] = {.srcStageMask = S::eAllCommands,
                        .srcAccessMask = A::eMemoryRead,
                        .dstStageMask = S::eComputeShader,
                        .dstAccessMask = A::eShaderStorageWrite,
                        .oldLayout = L::eUndefined,
                        .newLayout = L::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = slot.image[e].image,
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = b.data()});
        const std::array<VrEyeUpscale::Source, 2> sources{
            {{*slot.input_view[0], in[0]}, {*slot.input_view[1], in[1]}}};
        const std::array<VrEyeUpscale::Target, 2> targets{
            {{slot.image[0].image, *slot.view[0], {}, out[0]},
             {slot.image[1].image, *slot.view[1], {}, out[1]}}};
        bool ok = upscale.Record(cmd, sources, L::eShaderReadOnlyOptimal, targets, sharpen, nullptr,
                                 {}, bgra);
        for (u32 e = 0; e < 4; ++e) {
            b[e].srcStageMask = S::eComputeShader;
            b[e].srcAccessMask = e < 2 ? A::eShaderSampledRead : A::eShaderStorageWrite;
            b[e].dstStageMask = S::eAllTransfer;
            b[e].dstAccessMask = A::eTransferRead;
            b[e].oldLayout = b[e].newLayout;
            b[e].newLayout = L::eTransferSrcOptimal;
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = b.data()});
        for (u32 e = 0; e < 2; ++e)
            result[e] = slot.image[e].image;
        return ok;
    }
};
} // namespace Vulkan
