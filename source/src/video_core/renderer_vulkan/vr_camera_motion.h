// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_motion_camera.h"
namespace Vulkan {
class Instance;
class Scheduler;
class VrCameraMotion {
public:
    VrCameraMotion(const Instance& instance, Scheduler& scheduler);
    ~VrCameraMotion();
    struct Eye {
        vk::ImageView depth{}, object{}, motion_view{}; // GENERAL, single-layer 2D views
        vk::Image motion{}; // RG16F, one layer; caller keeps every image alive through submission
        VrMotionCamera::Constants camera{};
    };
    // Both eyes are validated before any commands are recorded. This composes camera vectors
    // with the matching G-buffer object vectors. No allocations/dispatches while disabled.
    // `twin` (FAST_DLSS_DIRECT_VERIFY, bit 46; may be null): a third pass with its own output,
    // the DLSS twin's motion from the copied depth.
    bool Record(const VR::Graphics::Config& accepted, bool provider_ready,
                const std::array<Eye, 2>& eyes, const Eye* twin);

private:
    bool Init();
    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    bool failed{};
};
} // namespace Vulkan
