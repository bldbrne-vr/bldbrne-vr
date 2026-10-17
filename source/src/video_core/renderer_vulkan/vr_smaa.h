// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class RecorderProxy;
class Scheduler;

// SMAA 1x in place of the game's FXAA (vr.ini aa_mode=smaa; core/vr/aa_mode.h, the decision;
// host_shaders/vr_smaa.comp, the reference passes as compute shaders). The rasterizer calls Record
// at the game's FXAA draw instead of drawing it, in the game's command stream: the FXAA draw's
// texture B (a copy of the scene colour the game made just before) is anti-aliased into its target
// A, which the composite, the HUD, our DIBR capture, the late warp and the eyes' TAA then read
// like the FXAA result they replace. Three dispatches at the render size (luma edges, blending
// weights, neighbourhood blending) through two private images (edges RG8, weights RGBA8) and
// SMAA's two lookup textures (AreaTex RG8 160x560, SearchTex R8 64x16, uploaded once from the
// reference's headers, renderer_vulkan/smaa). The game's images are read and written through our
// own views of them (texture-cache images are MUTABLE_FORMAT with storage usage): B as
// R8G8B8A8_UNORM (edges in gamma space) and R8G8B8A8_SRGB (blending in linear light), A as an
// R8G8B8A8_UNORM storage image (8-bit 4-channel images only: RGBA or BGRA bytes, the same order
// in both). Any Vulkan failure disables it for the session (one log line): the rasterizer then
// draws the game's FXAA as before.
class VrSmaa {
public:
    VrSmaa(const Instance& instance, Scheduler& scheduler);
    ~VrSmaa();
    VrSmaa(const VrSmaa&) = delete;
    VrSmaa& operator=(const VrSmaa&) = delete;

    [[nodiscard]] bool Ready() const {
        return ready;
    }

    struct Frame {
        vk::Image src{}; // B, in eShaderReadOnlyOptimal, its writes visible to compute
        vk::Format src_format{};
        u64 src_uid{};
        vk::Image dst{}; // A, in eGeneral, earlier accesses done (the caller's Transit)
        vk::Format dst_format{};
        u64 dst_uid{};
        vk::Extent2D extent{}; // both
        u32 layers{1};         // one mono layer or the native stereo pair
        bool swap_rb{false};   // written through a red / blue component swap (Bloodborne's)
        // FAST bit 41 (the rasterizer's SmaaRecorder): a second pair of intermediates for the
        // second eye, so each pass runs for both eyes between the same two barriers (3 barriers
        // per stereo frame instead of 6). One layer records as before.
        bool per_eye_targets{false};
        // The FXAA draw's render pass began A with a clear (its fast-clear metadata): A no
        // longer holds the scene, so FAST_GPU_SMAA_TRIM's keep cannot leave pixels to it.
        bool dst_cleared{false};
    };

    // FAST bit 41 statistics (the rasterizer's "FAST smaa_recorder" line). The first three are
    // written by the recording thread; the verify results arrive on the scheduler's
    // priority-operation thread (atomics).
    struct Stats {
        u64 per_eye_frames;    // stereo frames recorded with per-eye intermediates
        u64 barrier_calls;     // pipelineBarrier2 calls of the passes (6 / 3 per stereo frame)
        u64 verify_recorded;   // FAST_SMAA_RECORDER_VERIFY readback pairs recorded
        u64 verify_compared;   // ... compared after the GPU finished them
        u64 verify_mismatched; // ... that differed (an error line each)
    };
    [[nodiscard]] Stats GetStats() const;

    // The byte order of an 8-bit 4-channel format: 0 = R G B A, 1 = B G R A, -1 = not one.
    [[nodiscard]] static int ByteOrder(vk::Format format);

    // Records the three passes into `cmdbuf` (outside a render pass). False (logged once per
    // reason): nothing recorded, the caller draws the FXAA instead. Every command goes through
    // the proxy: encoded for the recorder thread, or straight into the command buffer while the
    // recorder is off or a raw handle is borrowed (the caller's choice, FAST bit 41).
    // FAST_GPU_SMAA_TRIM (off by default; vr_smaa.cpp) trims the weights and blend passes.
    bool Record(const RecorderProxy& cmdbuf, const Frame& frame);

private:
    struct Views {
        vk::Image image{};
        u64 uid{};
        vk::ImageView unorm{}; // R8G8B8A8_UNORM (sampled for B, storage for A)
        vk::ImageView srgb{};  // R8G8B8A8_SRGB (B only)
        u32 layer{};
        bool sampled{};
        u64 last_use{};
    };
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Extent2D extent{};
        vk::Format format{};
        bool fresh{true}; // still eUndefined
    };

    bool Init();
    bool EnsureTargets(vk::Extent2D extent);
    bool EnsureTarget(Target& t, vk::Extent2D extent, vk::Format format, const char* name);
    const Views* ViewsOf(vk::Image image, u64 uid, vk::Format format, bool srgb, u32 layer);
    void Upload(const RecorderProxy& cmdbuf);
    void Fail(const char* what);
    // FAST_GPU_SMAA_TRIM's "FAST smaa_trim" line (every 600 SMAA frames while a trim runs).
    void ReportTrim(bool keep, bool compact) const;

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;
    bool ready{false};
    bool failed{false};
    bool uploaded{false};
    u64 frame{0};
    vk::Format edges_format{vk::Format::eR8G8Unorm};
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline edges_pipeline;
    vk::UniquePipeline weights_pipeline;
    vk::UniquePipeline blend_pipeline;
    // FAST_GPU_SMAA_TRIM's variants (vr_smaa.cpp; built only when the switch asks for them,
    // null if they failed: the full pass runs): the blend that leaves A alone where no weight
    // applies, and the weights pass with its edge pixels packed into 16x16 groups' first warps.
    vk::UniquePipeline blend_keep_pipeline;
    vk::UniquePipeline weights_compact_pipeline;
    VideoCore::UniqueImage area_image;
    vk::UniqueImageView area_view;
    VideoCore::UniqueImage search_image;
    vk::UniqueImageView search_view;
    Target edges;
    Target weights;
    // FAST bit 41: the second eye's intermediates (created at the first such frame, then kept).
    Target edges_eye1;
    Target weights_eye1;
    std::vector<Views> views; // the game images' views (B and A: two in practice)
    u32 warned{0};
    u64 per_eye_frames{0};
    u64 barrier_calls{0};
    u64 verify_tick{0};
    u64 verify_recorded{0};
    // FAST_GPU_SMAA_TRIM (the recording thread).
    u64 trim_frames{0};          // frames recorded with a trim
    u64 trim_verify_tick{0};     // FAST_GPU_SMAA_TRIM_VERIFY's sampling
    u64 trim_verify_recorded{0}; // its readback pairs recorded
};

} // namespace Vulkan
