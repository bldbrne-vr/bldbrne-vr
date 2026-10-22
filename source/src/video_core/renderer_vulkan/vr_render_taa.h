// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "core/vr/render_taa.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

// aa_mode=taa: temporal AA in the game frame (core/vr/aa_mode.h, the decision; core/vr/render_taa.h,
// the maths; host_shaders/vr_render_taa.comp, the resolve). The rasterizer calls Record at the
// game's FXAA draw instead of drawing it: B (the game's copy of the tonemapped scene, rendered with
// this frame's sub-pixel jitter) is resolved into A in the unjittered pixel grid, with a history
// reprojected through the frame's depth and the exact previous / current render cameras. Two
// RGBA16F history images (rgb + the accumulation count) and two R32F stable depths (for the DIBR
// prep: an edge pixel keeps its side while the jitter moves the edge) ping-pong at the render
// size. The game's images are read and written through our own views (as VrSmaa). Any Vulkan
// failure disables it for the session (one log line): the rasterizer then uses SMAA / the FXAA.
class VrRenderTaa {
public:
    VrRenderTaa(const Instance& instance, Scheduler& scheduler);
    ~VrRenderTaa();
    VrRenderTaa(const VrRenderTaa&) = delete;
    VrRenderTaa& operator=(const VrRenderTaa&) = delete;

    [[nodiscard]] bool Ready() const {
        return ready;
    }

    struct Frame {
        vk::Image src{}; // B, in eShaderReadOnlyOptimal
        vk::Format src_format{};
        u64 src_uid{};
        vk::Image dst{}; // A, in eGeneral
        vk::Format dst_format{};
        u64 dst_uid{};
        vk::ImageView depth_view{}; // the game's depth, eDepthStencilReadOnlyOptimal
        vk::Extent2D depth_extent{}; // its read size (= extent, or a reduced one of its aspect)
        vk::Extent2D extent{};
        float jitter_x{0.0f}, jitter_y{0.0f}; // window px (B[q] = scene at q + 0.5 - jitter)
        bool y_down{true};                    // window y grows as view y falls
        VR::RenderTaa::Reprojector reproj{};
        float fx{1.0f}, fy{1.0f}, pfx{1.0f}, pfy{1.0f};
        VR::RenderTaa::DepthAffine depth{};
        VR::RenderTaa::DepthAffine prev_depth{};
        bool history{false}; // the previous frame's history is this camera's predecessor
        float alpha_min{0.1f};
        float gamma{1.0f};
        float depth_reject{0.01f};
        float sharpen{0.0f}; // > 0: RCAS of the new history into A (a second pass)
        float filter_px{0.45f}; // the current frame's Gaussian (render px)
        float near_m{0.0f};     // the near field (weapon, hands): 0 = off
        float near_alpha{0.8f}; // ... its least current-frame share
        float luma_weight{1.0f}; // the blend's anti-flicker luma weight k (0 = plain)
        bool occluder{false};    // the occluder test (an enemy's leading edge)
        float sharpen_lift{0.0f}; // RCAS's black floor moved down for the limit (dark scenes)
        u32 debug{0};
    };

    // Records the resolve into `cmdbuf` (outside a render pass). False (logged once per reason):
    // nothing recorded.
    bool Record(vk::CommandBuffer cmdbuf, const Frame& frame);

    // The stable depth written by the last Record (eGeneral, readable by compute), its size.
    [[nodiscard]] vk::ImageView StableDepthView() const;
    [[nodiscard]] vk::Extent2D Extent() const {
        return extent;
    }
    [[nodiscard]] u64 HistoryBytes() const;

private:
    struct Views {
        vk::Image image{};
        u64 uid{};
        vk::ImageView unorm{};
        u64 last_use{};
    };
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        bool fresh{true};
    };

    bool Init();
    bool EnsureTargets(vk::Extent2D extent);
    bool MakeTarget(Target& t, vk::Extent2D extent, vk::Format format, const char* name);
    const Views* ViewsOf(vk::Image image, u64 uid, bool storage);
    void Fail(const char* what);

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;
    bool ready{false};
    u64 frame{0};
    u32 cur{0}; // the history / stable depth written last
    vk::Extent2D extent{};
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniquePipeline sharpen_pipeline;
    std::array<Target, 2> hist;
    std::array<Target, 2> zst;
    std::vector<Views> views;
    u32 warned{0};
};

} // namespace Vulkan
