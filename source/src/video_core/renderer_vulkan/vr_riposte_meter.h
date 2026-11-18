// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// The riposte burst's brightness meter (vr.ini riposte_style; host_shaders/vr_riposte_meter.comp).
// While the burst shows, one small dispatch a frame measures both eyes' mean brightness into a
// 16-byte buffer, eased from the last frame's value. The passes that draw the burst read it
// (vr_riposte.glsl: where the bands cut): one value for both eyes, holding still while the head
// moves. The native eyes' feedback pass (native_present.inl) and the late warp's eye upscale
// (VrEyeUpscale) each own one.
class VrRiposteMeter {
public:
    struct Eye {
        vk::ImageView view;     // sampled
        vk::ImageLayout layout; // GENERAL or SHADER_READ_ONLY_OPTIMAL, readable by compute
        vk::Rect2D rect;        // the eye's texels in it
    };

    VrRiposteMeter() = default;
    ~VrRiposteMeter();
    VrRiposteMeter(const VrRiposteMeter&) = delete;
    VrRiposteMeter& operator=(const VrRiposteMeter&) = delete;

    // False (logged): no meter, so the burst is not drawn.
    bool Init(vk::Device device, VmaAllocator allocator);
    // The caller has waited for its GPU work.
    void Shutdown();
    [[nodiscard]] bool Ready() const {
        return ready;
    }
    [[nodiscard]] vk::Buffer Buffer() const {
        return buffer;
    }

    // Measures both eyes into Buffer() for the passes after it, which read it at `readers`.
    // The eyes' writes must already be visible to compute reads. `srgb_bytes`: the views hold
    // sRGB bytes read as UNORM (decoded as vr_eye_upscale.comp's taps decode them); `bgra`: in
    // B8G8R8A8 order. A burst's first frame (nothing measured for 0.25 s) takes its own measure;
    // later frames ease toward theirs.
    void Record(vk::CommandBuffer cmd, const std::array<Eye, 2>& eyes, bool srgb_bytes, bool bgra,
                vk::PipelineStageFlags2 readers);

private:
    vk::Device device;
    VmaAllocator allocator{};
    bool ready{false};
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    vk::Buffer buffer;
    VmaAllocation allocation{};
    s64 last_ns{0};
};

} // namespace Vulkan
