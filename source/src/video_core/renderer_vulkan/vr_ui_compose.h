// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#ifdef ENABLE_OPENXR
#include <array>
#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_ui_geometry.h"

namespace Vulkan {
// Frame-thread compositor. Records only into acquired, waited images, before XR release.
// Two fenced slots; a busy GPU falls back to layers instead of blocking an XR refresh.
class VrUiCompose {
public:
    ~VrUiCompose();
    bool Init(vk::Device device, u32 queue_family, vk::Format format, vk::PhysicalDevice physical);
    void Shutdown();
    vk::CommandBuffer Begin();
    bool Submit(vk::Queue queue, vk::Semaphore signal = {}, u64 value = 0);
    double LastGpuMs() const { return last_gpu_ms; }
    u32 SlotIndex() const { return current; }
    void Draw(vk::CommandBuffer cmd, vk::ImageView target, vk::Extent2D extent,
              vk::ImageView atlas, vk::Extent2D atlas_extent,
              std::span<const VrUiGeometry::Piece> pieces, const XrPosef& eye,
              const XrFovf& fov, const XrPosef& render_head);
private:
    struct Slot { vk::CommandBuffer cmd{}; vk::Fence fence{}; bool pending{}; };
    vk::Device device{};
    vk::Format format{};
    vk::CommandPool pool{};
    vk::DescriptorSetLayout set_layout{};
    vk::PipelineLayout layout{};
    vk::Pipeline pipeline{};
    vk::Sampler sampler{};
    std::array<Slot, 2> slots{};
    u32 current{};
    vk::QueryPool query_pool{};
    float timestamp_period{};
    u64 timestamp_mask{~u64{0}};
    double last_gpu_ms{};
};
} // namespace Vulkan
#endif
