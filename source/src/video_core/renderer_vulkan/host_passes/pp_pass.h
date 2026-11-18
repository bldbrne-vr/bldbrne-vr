//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_kos_iris.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        float health = 0.0f;
        float desaturation = 0.0f;
        std::array<float, 4> color_adjust{1.0f, 1.0f, 1.0f, 0.0f};
        std::array<s32, 2> source_offset{-1, -1};
        float riposte = 0.0f; // native VR: a visceral's burst (core/vr/riposte_style.h)
        u32 roar_palette = 0;
        // Native VR: the Kos ritual's vision (core/vr/trick_weapons.h KosRitualVisionAt; rise,
        // burst, clock, seed), pixel dimensions and the submitted eye's asymmetric FOV.
        std::array<float, 4> ritual{};
        std::array<float, 4> ritual_view{}; // width, height, unused, unused
        std::array<float, 4> ritual_fov{}; // tangents: left, right, up, down
    };

    static_assert(sizeof(Settings) == 96);

    void Create(vk::Device device, vk::Format surface_format, bool native_color = false, VmaAllocator allocator = {});

    // `riposte_meter` (native_color passes; VrRiposteMeter's buffer, this frame's): with
    // settings.riposte > 0, the burst variant draws the burst with it.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::Buffer riposte_meter = {});

private:
    VrKosIris kos_iris;
    vk::AttachmentLoadOp load_op{vk::AttachmentLoadOp::eClear};
    // [colour + 2 * burst]: the burst variants (VR_RIPOSTE) only for native_color.
    std::array<vk::UniquePipeline, 4> pipelines{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
