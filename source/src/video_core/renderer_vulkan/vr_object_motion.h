// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <memory>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_object_motion_state.h"
#include "video_core/texture_cache/image.h"
namespace Vulkan {
class Instance;
class Scheduler;
struct RenderState;
// Lazy native-stereo counterpart to bbport ObjectMotion. Only the frame owner calls it.
// Neither constructing this object nor an OFF Begin allocates motion buffers or images.
class VrObjectMotion {
public:
    VrObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~VrObjectMotion();
    bool Begin(const VR::Graphics::Config& accepted, bool provider_ready, uint64_t frame,
               uint64_t epoch, bool camera_cut = false);
    Shader::VrMotion Specialization() const;
    uint32_t Prepare(const Motion::Draw& draw);
    // Select the pipeline's motion variant only when Attach succeeds. It requires a native
    // multiview pass with attachment 7 free, and does not modify the guest attachments.
    bool Attach(RenderState& state);
    std::array<vk::ImageView, 2> PrepareRead(uint32_t width, uint32_t height);
    bool Active() const {
        return active;
    }

private:
    // Start selective tracking at 4M positions/eye; grow after exhaustion, up to
    // the original broad-tracking budget. Never silently truncate history.
    static constexpr uint32_t PositionsPerEye = 16u << 20, MaxTargets = 4;
    uint32_t position_capacity{};
    bool grow_pending{};
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView array_view;
        std::array<vk::UniqueImageView, 2> eye_views;
        uint32_t width{}, height{};
        uint64_t used_frame{};
        bool written{};
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    };
    bool Init();
    void Release();
    Target* GetTarget(uint32_t width, uint32_t height);
    void Retire(Target& target);
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VrObjectMotionState> history;
    vk::Buffer parameters{}, positions{};
    VmaAllocation parameters_allocation{}, positions_allocation{};
    Shader::VrMotion::Params* mapped{};
    Shader::VrMotion specialization{};
    std::array<Target, MaxTargets> targets;
    uint64_t frame{};
    bool active{}, failed{};
};
} // namespace Vulkan
