// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/vr/color_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// The eyes' upscale and sharpening before the headset (vr.ini eye_upscale / eye_sharpen).
// The DIBR and late-warp eyes have the render's pixels
// (9 - 12 px per degree at the centre with the 4:3 presets); the compositor used to enlarge them
// bilinearly to the headset (~25 px per degree on a Quest 3), a soft and blocky stretch. Each eye
// is enlarged once here with FSR 1's EASU (an edge-directed Lanczos-2), then sharpened with its
// RCAS when `sharpen` > 0, so the compositor's own resample is close to 1:1.
// host_shaders/vr_eye_upscale.comp. Pure Vulkan: the late warp (frame thread) and the presenter's
// DIBR copy (GPU thread) each own an instance.
class VrEyeUpscale {
public:
    struct Source {
        vk::ImageView view;  // one eye at the render's pixels, sampled (its own image)
        vk::Extent2D extent; // the whole image
    };
    struct Target {
        vk::Image image;     // storage, in GENERAL; may be the same image for both eyes
        vk::ImageView view;
        vk::Offset2D offset; // the eye's rect in it
        vk::Extent2D extent; // >= the source's (equal: sharpening only)
    };
    // vr.ini vignette_in_warp (the late warp): the uniform block of the frame's vignettes
    // (VrVignetteWarp::Block, host-written before the submit), blended into each eye by the
    // dispatch that writes it last.
    struct Vignettes {
        vk::Buffer buffer;
        vk::DeviceSize offset{0};
        vk::DeviceSize range{0};
    };

    VrEyeUpscale() = default;
    ~VrEyeUpscale();
    VrEyeUpscale(const VrEyeUpscale&) = delete;
    VrEyeUpscale& operator=(const VrEyeUpscale&) = delete;

    // Compiles both pipelines. False (logged): the caller copies the eyes as they are.
    bool Init(vk::Device device, VmaAllocator allocator);
    // The caller has waited for its GPU work.
    void Shutdown();
    [[nodiscard]] bool Ready() const {
        return ready;
    }
    // The vignette variants compiled (Record can take Vignettes).
    [[nodiscard]] bool VignettesReady() const {
        return ready && easu_vig[0] && easu_vig[1] && easu_vig[2] &&
               rcas_vig[0] && rcas_vig[1] && rcas_vig[2];
    }

    // The output size of an eye of `in` pixels at `scale` (1 = the same), even, at most 4x.
    [[nodiscard]] static vk::Extent2D Scaled(vk::Extent2D in, float scale);
    // Whether Record has anything to do (else: copy the eyes as they are).
    [[nodiscard]] static bool Active(float scale, float sharpen) {
        return scale > 1.0f + 1e-3f || sharpen > 0.0f;
    }

    // Allocates what Record needs for these sizes (the intermediate image when both passes run).
    // False (logged once): do not Record this frame.
    bool Prepare(const std::array<vk::Extent2D, 2>& inputs, const std::array<vk::Extent2D, 2>& outputs,
                 float sharpen);

    // Records both eyes: EASU from each source to its target rect when it is larger, then RCAS
    // (`sharpen` 0..1 = the linear strength of its sharpening lobe; 0 = none). The sources must be
    // readable by compute shaders in `source_layout` (GENERAL or SHADER_READ_ONLY_OPTIMAL) and
    // their writes made visible to compute reads; the targets in GENERAL with any earlier access
    // to them finished (the caller's barrier). Afterwards the targets' storage writes are
    // available at the compute stage (the caller's barrier makes them visible to its copy).
    // False: nothing recorded (not ready, or no intermediate image: logged once).
    // `vignettes` (VignettesReady() only; null: none): blended into each eye by its last pass.
    bool Record(vk::CommandBuffer cmd, const std::array<Source, 2>& sources,
                vk::ImageLayout source_layout, const std::array<Target, 2>& targets,
                float sharpen, const Vignettes* vignettes = nullptr,
                VR::Color::Settings color = {}, bool bgra = false, float output_gamma = 1.0f);

private:
    struct Image {
        vk::Image image;
        VmaAllocation allocation{};
        vk::ImageView view;
        vk::Extent2D extent{};
    };
    struct Retired {
        Image image;
        u32 calls{};
    };
    static constexpr u32 RetireCalls = 16;
    bool EnsureMid(vk::Extent2D extent);
    void DestroyImage(Image& image);
    void DestroyMid();

    vk::Device device;
    VmaAllocator allocator{};
    bool ready{false};
    bool warned{false};
    bool logged{false}; // the first EASU dispatch logged
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    std::array<vk::Pipeline, 3> easu{};
    std::array<vk::Pipeline, 3> rcas{};
    std::array<vk::Pipeline, 3> easu_vig{}; // VR_VIGNETTE variants (vignette_in_warp)
    std::array<vk::Pipeline, 3> rcas_vig{};
    Image mid{};                 // EASU's output when RCAS follows (both eyes side by side)
    std::vector<Retired> retired; // smaller `mid`s work in flight may still read
};

} // namespace Vulkan
