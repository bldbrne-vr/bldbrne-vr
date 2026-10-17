//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

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
    };

    static_assert(sizeof(Settings) == 40);

    void Create(vk::Device device, vk::Format surface_format, bool native_color = false);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

private:
    vk::AttachmentLoadOp load_op{vk::AttachmentLoadOp::eClear};
    std::array<vk::UniquePipeline, 2> pipelines{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
