// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <span>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Image;
struct UniqueBuffer;
}
namespace Vulkan {
class Instance;
class Scheduler;
class RecorderProxy;

// The scene depth and the HUD's stencil masks at the output size where the card cannot blit
// their format: AMD has no BLIT_DST for any depth/stencil format (RX 6700, RX 9070 XT), which
// left the S1 upscalers without our in-frame items and the HUD layer incomplete there (the
// game's flat HUD stayed in both eyes). Draw: a triangle over the target writes it as its
// depth/stencil attachment from the sampled source (vr_depth_copy.frag: gl_FragDepth;
// vr_stencil_copy.frag: the stencil through VK_EXT_shader_stencil_export, which AMD has), the
// way DXVK copies depth/stencil images. Buffer, the fallback (no stencil export, or a source
// that cannot be sampled): each aspect goes into a buffer (copyImageToBuffer), a compute pass
// scales it (vr_depth_rescale.comp) and copyBufferToImage writes the target. RTX 5080 with
// VR_TEST_NO_DEPTH_BLIT=1 (heavy sewers, FSR 3.1.5, the HUD's 9 copies a frame): GPU 7.25 ms a
// frame with blits, 8.96 with buffer copies only, 8.31 with the depth drawn (no stencil
// export there), ~7.6 with the stencil drawn as well (timed with a depth draw in its place).
class VrDepthRescale {
public:
    enum class Mode { Blit, Draw, Buffer, None };
    VrDepthRescale(const Instance& instance, Scheduler& scheduler);
    ~VrDepthRescale();
    // A nearest blit can scale this depth/stencil format. VR_TEST_NO_DEPTH_BLIT=1: never, so
    // another card takes AMD's path (tests, A/B).
    static bool Blits(const Instance& instance, vk::Format format);
    // Record can scale every aspect of this depth/stencil format.
    static bool Scales(vk::Format format);
    // How `aspects` of `src` are scaled into an image of its format, and the layouts that needs:
    // src TRANSFER_SRC_OPTIMAL and the target TRANSFER_DST_OPTIMAL (Blit, Buffer), or src
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL and the target DEPTH_STENCIL_ATTACHMENT_OPTIMAL (Draw; its
    // image needs DEPTH_STENCIL_ATTACHMENT usage). VR_TEST_DEPTH_COPY=buffer: never Draw (A/B).
    static Mode Choose(const Instance& instance, const VideoCore::Image& src,
                       vk::ImageAspectFlags aspects);
    // Buffer: layers [0, layers) of `aspects`, src in TRANSFER_SRC_OPTIMAL and dst in
    // TRANSFER_DST_OPTIMAL (as for the blit, which it replaces); outside a render pass. The
    // destination is written by transfer when this returns true; false (an aspect the format
    // lacks, or the pipeline failed) records nothing. A failed buffer allocation asserts, as
    // for every buffer (VideoCore::UniqueBuffer).
    bool Record(const RecorderProxy& cmd, vk::Image src, vk::Extent2D src_size, vk::Image dst,
                vk::Extent2D dst_size, uint32_t layers, vk::Format format,
                vk::ImageAspectFlags aspects);
    // Draw, in Choose's layouts, outside a render pass and outside a draw's binds (it binds a
    // graphics pipeline, set 0, push constants and a viewport of its own; the game's dynamic
    // state and push memo are invalidated after it). One pass per layer. Of the target's aspects
    // not drawn, those in `keep` are kept (loaded and stored); the rest are left undefined, with
    // no memory traffic (the HUD twin's depth when only its masks are valid). `targets`:
    // a depth/stencil view per layer of dst, made here when empty (their owner destroys them
    // with dst). False: nothing recorded (an aspect whose pipeline failed then falls back to
    // the buffer copy from the next Choose on).
    bool Draw(const RecorderProxy& cmd, VideoCore::Image& src, vk::Extent2D src_size,
              vk::Image dst, vk::Extent2D dst_size, uint32_t layers, vk::Format format,
              vk::ImageAspectFlags aspects, std::span<vk::UniqueImageView> targets,
              vk::ImageAspectFlags keep);

private:
    bool Init();
    bool Buffers(vk::DeviceSize src_bytes, vk::DeviceSize dst_bytes);
    struct DrawPipelines {
        vk::Format format{};
        vk::UniquePipeline depth, stencil; // stencil: only with VK_EXT_shader_stencil_export
    };
    const DrawPipelines* DrawPipelinesFor(vk::Format format);
    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    std::unique_ptr<VideoCore::UniqueBuffer> src_buffer, dst_buffer;
    vk::DeviceSize src_capacity{}, dst_capacity{};
    bool failed{};
    vk::UniqueDescriptorSetLayout draw_descriptors;
    vk::UniquePipelineLayout draw_layout;
    std::vector<DrawPipelines> draw_pipelines;
    bool draw_failed{};
};
} // namespace Vulkan
