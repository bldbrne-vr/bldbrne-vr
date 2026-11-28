// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "core/vr/inframe.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

class Instance;
class Scheduler;
class RecorderProxy;

// The in-frame renderer (core/vr/inframe.h): draws a camera record's list of our small things
// (the quick-item arc, labels, held fallbacks) into the game's own frame, in the game's command
// stream, right after the scene composite and before the DIBR capture and the HUD:
//  - the scene depth is copied into a private depth image first; our items are depth-tested and
//    written against that copy only, so the game's depth/stencil is never touched (the HUD's own
//    depth tests stay vanilla), and DIBR then reads the copy: our items get their own disparity;
//    under S1 (native stereo, the scene reconstructed at the output size) the depth is already
//    ours, scaled for us: the items go straight into its eye's layer (Target::depth_in_place);
//  - colour goes into the scene colour through an sRGB view (the image holds sRGB-encoded values,
//    as DIBR decodes it), linear blending, RGB only (the game's alpha is left alone);
//  - camera: the record's own view (the list is built in it) and its projection (vfov, aspect,
//    near, far: the game's D3D-style depth, not reversed), viewport depth = the scene's range.
// One pipeline (premultiplied blend, depth LEQUAL + write, no culling: the game's view space is
// left-handed; a second attachment takes the coverage, blended MAX, for the DIBR warp); push
// descriptors (one texture per draw), push constants (128 bytes per draw). Single-sampled after the
// game's AA: the cards' round edges are anti-aliased analytically in the shader and the icons and
// labels (one mip level) are box-filtered over each pixel's footprint.
// Light: the scene colour is box-filtered down to a coarse probe first (ProbeScenes: blits into a mip
// chain, each texel of its 5th level averages 64 x 64 scene pixels), and every fragment reads the
// scene's light behind it there: lit items darken in a dark room and take its tint, cards and text
// keep a floor so they stay readable.
// Textures: the game's icons (BC7, uploaded once each, up to 16), a label atlas (RG8: fill,
// halo; 16 rows) and the held items' game models (their own vertex / index buffers and BC
// diffuse textures with the game's mips, up to 8). Any Vulkan failure disables it for the
// session (one log line) and the frame goes on as if it did not exist.
class VrInFrame {
public:
    VrInFrame(const Instance& instance, Scheduler& scheduler);
    ~VrInFrame();
    VrInFrame(const VrInFrame&) = delete;
    VrInFrame& operator=(const VrInFrame&) = delete;

    /// At VR startup, start the mesh worker and compile the two shaders shared by both eyes.
    static void Prewarm();

    struct Target {
        vk::ImageView color_view;   // sRGB view of the scene colour (eColorAttachmentOptimal)
        vk::Format color_format{};  // that view's format
        bool swapped{false};        // the scene colour's bytes are B G R (the probe is read swapped)
        vk::Extent2D extent{};      // the scene colour's size
        vk::Image depth_image;      // the scene depth (eTransferSrcOptimal)
        vk::Format depth_format{};
        vk::Extent2D depth_extent{};
        float zmin{0.0f};
        float zmax{1.0f};
        u32 depth_layer{0};
        // Set: `depth_image` is a scene depth of our own (S1's output-size twin), and this is its
        // layer `depth_layer` as a depth/stencil attachment view. Our items then test and write
        // it in place: no private copy (none is even made), and DepthView() is not this frame's
        // (only the DIBR capture reads it, and S1 is native stereo).
        vk::ImageView depth_in_place{};
    };
    struct Camera {
        float vfov{1.0f}; // radians
        float aspect{1.0f};
        float near_z{0.1f}; // game units
        float far_z{3000.0f};
        float eye_x{0.0f}; // eye offset in the list's view space, in game units
        s32 monocular_eye{-1}; // native stereo: the eye looking through the held Monocular
        std::array<float, 4> clip{1.0f, 1.0f, 0.0f, 0.0f}; // Vulkan clip xy scale/offset
        bool collect_probe{true}; // one arm-fit result per frame
        u32 model_eye_mask{1}; // staged sever models: mono/DIBR 1, native stereo 3
    };

    // Records the depth copy, the texture uploads and the draws (outside a render pass). False:
    // nothing was recorded (an empty list, a refusal, disabled). On true the private depth copy
    // holds the scene depth plus ours, in eDepthStencilReadOnlyOptimal, for DepthView() (in place:
    // the target's layer holds them, in eDepthStencilAttachmentOptimal).
    // Runtime models dropped (a severed piece gone): their GPU copies freed. Once a frame, after
    // both eyes have drawn (Rasterizer::VrEndFrame), drawn or not: never between the eyes, so a
    // piece retired mid-frame goes from both at once (a code review, 2026-11-13). One test while
    // this eye holds none.
    void SweepDropped();
    bool Record(const RecorderProxy& cmdbuf, const Target& target, const Camera& camera,
                const VR::InFrame::RenderList& list);
    // The private depth copy, sampled (depth aspect), after a Record that returned true and
    // HasDepthCopy().
    [[nodiscard]] vk::ImageView DepthView() const {
        return *depth_sampled_view;
    }
    // The last Record drew into the private depth copy (not in place).
    [[nodiscard]] bool HasDepthCopy() const {
        return depth_copied;
    }
    // After a Record that returned true: our items' coverage (R8, the depth copy's size, 0 where
    // nothing of ours is), in eTransferSrcOptimal. The DIBR warp keeps the HUD's edges from
    // cutting our items with it (VrStereoWarp::CaptureItemMask).
    [[nodiscard]] vk::Image CoverImage() const {
        return cover;
    }
    [[nodiscard]] vk::Extent2D CoverExtent() const {
        return cover_extent;
    }
    // Before Record, outside a render pass: the scene's light into each eye's probe (`src` = the
    // scene colour in eTransferSrcOptimal, `src_format` its own format; each eye's from its
    // `source_layer`). The eyes' mip chains go level by level, one barrier per level for all of
    // them. An eye without a probe (too small, a failure) lights its items with a neutral scene.
    // `inset`: the probe is made from `src` less that many pixels on each side (dynamic
    // resolution's view, for items drawn at the headset's projection); its size stays half of
    // `extent` either way.
    struct ProbeSource {
        VrInFrame* renderer{};
        u32 source_layer{0};
    };
    static void ProbeScenes(const RecorderProxy& cmdbuf, std::span<const ProbeSource> eyes,
                            vk::Image src, vk::Format src_format, vk::Extent2D extent,
                            vk::Offset2D inset = {});
    // Creates the pipeline for these formats ahead of the first list (no hitch at the first use),
    // for this camera's planes (0: the usual ones); the translucent one on a worker.
    void Warm(vk::Format color_format, vk::Format depth_format, float near_z = 0.0f,
              float far_z = 0.0f);
    [[nodiscard]] bool Active() const {
        return !failed;
    }
    // Once every 10 s: a STATS line (lists drawn, items, labels, GPU and CPU time).
    void LogStats();

private:
    struct Buffer {
        vk::Buffer buffer;
        VmaAllocation allocation{};
        void* mapped{};
    };
    struct Texture {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u64 last_used{0};
    };
    struct LabelRow {
        std::string text;
        u32 width{0};
        u32 height{0};
        u64 last_used{0};
        bool valid{false};
    };
    struct MeshRange {
        u32 first_index{0};
        u32 index_count{0};
        s32 vertex_offset{0};
    };
    // A game model (VR::Assets::GetModel): its own buffers and diffuse texture (with the game's
    // mip chain), uploaded on first use, up to MaxModels (the least recently used goes).
    struct GpuModel {
        Buffer vertices{};
        Buffer indices{};
        Buffer animation{};
        vk::IndexType index_type{vk::IndexType::eUint32};
        VideoCore::UniqueImage surface;
        vk::UniqueImageView surface_view;
        u32 vertex_count{0};
        u32 animation_bones{16};
        u32 index_count{0};
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u64 shared_key{0};          // its texture is shared_images' (ModelTexture::share_key)
        vk::ImageView shared_view{}; // ... its view
        bool textured{false};
        bool single_surface{false};
        u64 last_used{0};
        u32 reveal_token{0}; // staged cut: pinned until visible or cancelled/dropped
        vk::ImageView View() const {
            return shared_key != 0 ? shared_view : *view;
        }
    };
    // A texture several models share (a severed piece's of one enemy, the flesh tile), kept
    // while any of them is.
    struct SharedImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 users{0};
    };

    // Record's list itself (after the frame's bookkeeping and the sweep).
    bool RecordList(const RecorderProxy& cmdbuf, const Target& target, const Camera& camera,
                    const VR::InFrame::RenderList& list);
    bool Fail(const std::string& what);
    bool CreateBuffer(Buffer& out, vk::DeviceSize size, vk::BufferUsageFlags usage);
    void DestroyBufferLater(Buffer& buffer);
    bool BuildMeshes();
    // `xray`: the pipeline of Item::xray (depth compare reversed, no depth write).
    // `depth`: the shader's specialized depth mapping {A, B} (DepthTerms). The eye's projection
    // is pushed per draw, so a moving projection (the Monocular's window) never builds one.
    vk::Pipeline PipelineFor(vk::Format color_format, vk::Format depth_format, bool xray,
                             const std::array<float, 2>& depth, bool translucent = false);
    // The pipeline itself (no cache, no log); empty on failure. Any thread (the device and the
    // shader modules only).
    vk::UniquePipeline CreatePipeline(vk::Format color_format, vk::Format depth_format, bool xray,
                                      const std::array<float, 2>& depth, bool translucent) const;
    // The translucent pipeline for this target and camera, built on a worker the first time it is
    // asked for (its build takes ~2 s: a world flash or a bolt would stall the frame and miss its
    // moment); null until it is ready. Asked every frame, so it is ready long before it is needed.
    vk::Pipeline TranslucentFor(vk::Format color_format, vk::Format depth_format,
                                const std::array<float, 2>& depth);
    // The private depth copy (not in place) and the coverage, of the target's depth size.
    bool EnsureDepth(const Target& target);
    bool EnsureCover(vk::Extent2D extent);
    // The probe image for this scene size and format; false: no probe this frame.
    bool PrepareProbe(vk::Format src_format, vk::Extent2D extent);
    vk::ImageView IconView(const RecorderProxy& cmdbuf, u32 icon_id);
    // The model's GPU copy (uploaded on first use); nullptr while it loads or when it is missing.
    const GpuModel* ModelOf(const RecorderProxy& cmdbuf, s32 model_id, u32 reveal_token = 0);
    void DestroyModelLater(GpuModel& model);
    // The atlas row of a label (rasterised and uploaded on first use); -1 when not ready.
    int LabelRowOf(const RecorderProxy& cmdbuf, const VR::InFrame::Label& label);
    bool Upload(const RecorderProxy& cmdbuf, vk::Image image, const void* data, size_t size,
                const vk::BufferImageCopy& region, bool first_use);
    void CollectTimes();
    // The depth probe (VR::InFrame::Probe): its texels of the game's depth copied for the CPU.
    void RecordProbe(const RecorderProxy& cmdbuf, const Target& t, const Camera& camera,
                     const VR::InFrame::RenderList& list);
    void CollectProbes();
    bool CreateReadback(Buffer& out, vk::DeviceSize size);

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;
    bool failed{false};
    bool bc7{false};
    u64 frame{0};

    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::ShaderModule vert;
    vk::ShaderModule frag;
    vk::UniqueSampler sampler;
    struct PipelineEntry {
        vk::Format color{};
        vk::Format depth{};
        bool xray{false};
        bool translucent{false};
        std::array<float, 2> depth_terms{}; // the specialized depth mapping {A, B}
        vk::UniquePipeline pipeline;
    };
    std::vector<PipelineEntry> pipelines;

    Buffer vertices{};
    Buffer indices{};
    std::array<MeshRange, static_cast<size_t>(VR::InFrame::MeshId::Count)> ranges{};

    // The private depth copy, and the coverage (the same size).
    static constexpr vk::Format CoverFormat = vk::Format::eR8Unorm;
    VideoCore::UniqueImage depth;
    vk::UniqueImageView depth_attachment_view;
    vk::UniqueImageView depth_sampled_view;
    vk::Format depth_format{};
    vk::Extent2D depth_extent{};
    bool depth_copied{false}; // HasDepthCopy
    u32 in_place_lists{0};    // in a row, while a private copy is still kept (EnsureDepth)
    VideoCore::UniqueImage cover;
    vk::UniqueImageView cover_view;
    vk::Extent2D cover_extent{};

    // Textures.
    std::unordered_map<u32, Texture> icons;
    std::unordered_map<s32, GpuModel> models;
    std::unordered_map<s32, bool> model_missing; // the game files have no such model
    std::unordered_map<u64, SharedImage> shared_images;
    u32 dropped_models{0}; // VR::Assets::DroppedModels as last checked
    bool runtime_held{false}; // a runtime model uploaded (or refused) and maybe not dropped yet
    u32 model_uploads{0};  // new models uploaded this frame (MaxModelUploadsPerFrame)
    vk::UniqueSampler model_sampler; // trilinear, repeat (the models' own uvs)
    VideoCore::UniqueImage white;    // 1 x 1 white: an untextured model
    vk::UniqueImageView white_view;
    bool white_ready{false};
    VideoCore::UniqueImage label_atlas;
    // The scene light probe (half the scene's size, ProbeLevels mips; read at its last level).
    static constexpr u32 ProbeLevels = 6;
    VideoCore::UniqueImage probe;
    vk::UniqueImageView probe_view;
    vk::Extent2D probe_extent{};
    vk::Format probe_format{};
    bool probe_valid{false}; // filled this frame
    vk::UniqueImageView label_view;
    bool label_atlas_ready{false};
    std::array<LabelRow, 16> label_rows{};

    // The depth probe's readback slots (one probe each, collected once the GPU is past them): the
    // depth in a box of at most ProbeBox x ProbeBox texels round the probe's points.
    static constexpr u32 ProbeSlots = 4;
    static constexpr u32 ProbeBox = 256;
    struct ProbeSlot {
        bool pending{false};
        u64 tick{0};
        float a{1.0f}, b{0.0f}, zmin{0.0f}, zmax{1.0f};
        std::array<s32, 4> box{}; // x, y, width, height
        std::array<bool, VR::InFrame::MaxProbe> texel{};
        std::array<s32, VR::InFrame::MaxProbe> tx{};
        std::array<s32, VR::InFrame::MaxProbe> ty{};
        VR::InFrame::ProbeResult result{};
    };
    Buffer probe_buffer{};
    std::array<ProbeSlot, ProbeSlots> probe_slots{};

    // GPU timing (timestamps around the pass, read a few frames later).
    vk::UniqueQueryPool query_pool;
    float timestamp_period_ns{0.0f};
    static constexpr u32 TimingSlots = 8;
    std::array<bool, TimingSlots> timing_pending{};
    u32 timing_slot{0};

    // Statistics since the last STATS line.
    u64 stats_lists{0};
    u64 stats_items{0};
    u64 stats_labels{0};
    u64 stats_uploads{0};
    double stats_gpu_us{0.0};
    u64 stats_gpu_samples{0};
    double stats_cpu_us{0.0};
    double stats_cpu_max_us{0.0};
    s64 stats_since_ns{0};

    // TranslucentFor's build on a worker: its key and its result (the destructor waits for it).
    struct PendingPipeline {
        vk::Format color{};
        vk::Format depth{};
        std::array<float, 2> depth_terms{};
        s64 start_ns{0};
        std::future<vk::UniquePipeline> result;
    };
    std::optional<PendingPipeline> pending_translucent;
    bool translucent_failed{false}; // a build failed: none is asked for again
};

} // namespace Vulkan
