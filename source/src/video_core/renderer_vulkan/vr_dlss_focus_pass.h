// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_dlss_bridge.h"
#include "video_core/renderer_vulkan/vr_dlss_focus.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

// Foveated upscaling's two passes around the upscaler (vr_dlss_focus.h), the same for both eyes:
//  - Crop, before it: each eye's rectangle cut out of the upscaler's whole-eye inputs (colour,
//    depth, motion) into images of its own, origin at the corner (vr_dlss_focus_crop.comp), and
//    an output of the rectangle's size: the upscaler gets them as it gets any whole eye.
//  - Compose, after it: each whole eye written from the upscaler's output inside the rectangle
//    and the render enlarged and steadied by a TAA around it, blended over a band
//    (vr_dlss_focus.comp). The TAA's history is the last composed eye: the eye's output and an
//    image of this pass's own (its size and format) take turns, one written while the other is
//    read, and the one written is the eye's output for that frame.
// The images are this pass's own (the rectangles' sizes per eye; made again when a size or
// format changes, the old ones freed after the GPU is done with them). One compute dispatch per
// eye and pass in the draw stream (push descriptors, push constants). Any Vulkan failure: one
// log line, the pass refuses from then on (the owner turns focus off: DlssFocus::Off).
class VrDlssFocusPass {
public:
    VrDlssFocusPass(const Instance& instance, Scheduler& scheduler);
    ~VrDlssFocusPass();
    VrDlssFocusPass(const VrDlssFocusPass&) = delete;
    VrDlssFocusPass& operator=(const VrDlssFocusPass&) = delete;

    // `eyes`: the provider frame's whole-eye images (colour, depth, motion sampled, GENERAL;
    // output: its format names the cut-out output's). On true they name this pass's cut-outs
    // (GENERAL), jitter and the rest unchanged; on false nothing was recorded or changed.
    bool Crop(std::array<BbDlssEvaluate, 2>& eyes, const std::array<DlssFocus::Rect, 2>& rects);

    struct Eye {
        vk::ImageView render; // the render (the upscaler's colour input), sampled, GENERAL
        vk::ImageView motion; // the whole eye's motion vectors (render pixels), sampled, GENERAL
        BbDlssImage dlss;     // the upscaler's output of the rectangle (Crop's), GENERAL
        BbDlssImage output;   // the whole eye output (RGBA8 or RGBA16F storage), GENERAL
        DlssFocus::Rect rect;
    };
    // `render_w/h`: the render's size; `jitter`: the frame's jitter (render pixels); `reset`: no
    // history this frame (the upscaler's own reset); `frame`: the frame's number (the history
    // only from the frame before). On true `written` names each eye's composed image (its output
    // or this pass's twin of it, GENERAL): the eye's output for this frame.
    bool Compose(const std::array<Eye, 2>& eyes, u32 render_w, u32 render_h,
                 std::array<float, 2> jitter, bool reset, u64 frame,
                 std::array<BbDlssImage, 2>& written);

private:
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Format format{};
        u32 width{}, height{};
        bool fresh{};
    };
    struct EyeTargets {
        Target color, depth, motion, output;
        Target history; // the whole eye output's twin (the TAA's turns)
        VkImage latest{}; // the image holding the last composed eye (the output or the twin)
        u64 latest_frame{}; // its frame
    };
    bool Init();
    // The images, freed once the GPU is done with them (the destructor, a failure).
    void Release();
    // A failure: refuse from now on (the owner turns focus off), the images freed.
    void Fail();
    bool Ensure(Target& t, vk::Format format, u32 width, u32 height, const char* name);
    static BbDlssImage Image(const Target& t);
    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout crop_set_layout, compose_set_layout;
    vk::UniquePipelineLayout crop_layout, compose_layout;
    // By format: the cut-out colour (crop) and the whole eye output (compose).
    vk::UniquePipeline crop_rgba8, crop_rgba16f, compose_rgba8, compose_rgba16f;
    std::array<EyeTargets, 2> targets;
    bool tried{}, failed{};
};

} // namespace Vulkan
