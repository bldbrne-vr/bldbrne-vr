// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <memory>
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
//  - colour goes into the scene colour through an sRGB view (the image holds sRGB-encoded values,
//    as DIBR decodes it), linear blending, RGB only (the game's alpha is left alone);
//  - camera: the record's own view (the list is built in it) and its projection (vfov, aspect,
//    near, far: the game's D3D-style depth, not reversed), viewport depth = the scene's range.
// One pipeline (premultiplied blend, depth LEQUAL + write, no culling: the game's view space is
// left-handed; a second attachment takes the coverage, blended MAX, for the DIBR warp); push
// descriptors (one texture per draw), push constants (128 bytes per draw). Single-sampled after the
// game's AA: the cards' round edges are anti-aliased analytically in the shader and the icons and
// labels (one mip level) are box-filtered over each pixel's footprint.
// Light: the scene colour is box-filtered down to a coarse probe first (ProbeScene: blits into a mip
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
    };
    struct Camera {
        float vfov{1.0f}; // radians
        float aspect{1.0f};
        float near_z{0.1f}; // game units
        float far_z{3000.0f};
        float eye_x{0.0f}; // eye offset in the list's view space, in game units
        std::array<float, 4> clip{1.0f, 1.0f, 0.0f, 0.0f}; // Vulkan clip xy scale/offset
        bool collect_probe{true}; // one arm-fit result per frame
    };

    // Records the depth copy, the texture uploads and the draws (outside a render pass). False:
    // nothing was recorded (an empty list, a refusal, disabled). On true the private depth copy
    // holds the scene depth plus ours, in eDepthStencilReadOnlyOptimal, for DepthView().
    bool Record(const RecorderProxy& cmdbuf, const Target& target, const Camera& camera,
                const VR::InFrame::List& list);
    // The private depth copy, sampled (depth aspect), after a Record that returned true.
    [[nodiscard]] vk::ImageView DepthView() const {
        return *depth_sampled_view;
    }
    // After a Record that returned true: our items' coverage (R8, the depth copy's size, 0 where
    // nothing of ours is), in eTransferSrcOptimal. The DIBR warp keeps the HUD's edges from
    // cutting our items with it (VrStereoWarp::CaptureItemMask).
    [[nodiscard]] vk::Image CoverImage() const {
        return cover;
    }
    [[nodiscard]] vk::Extent2D CoverExtent() const {
        return depth_extent;
    }
    // Before Record, outside a render pass: the scene's light into the probe (`src` = the scene
    // colour in eTransferSrcOptimal, `src_format` its own format). False: Record lights items
    // with a neutral scene.
    bool ProbeScene(const RecorderProxy& cmdbuf, vk::Image src, vk::Format src_format,
                    vk::Extent2D extent, u32 source_layer = 0);
    // Creates the pipeline for these formats ahead of the first list (no hitch at the first use).
    void Warm(vk::Format color_format, vk::Format depth_format);
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
        u32 index_count{0};
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        bool textured{false};
        u64 last_used{0};
    };

    bool Fail(const std::string& what);
    bool CreateBuffer(Buffer& out, vk::DeviceSize size, vk::BufferUsageFlags usage);
    void DestroyBufferLater(Buffer& buffer);
    bool BuildMeshes();
    // `xray`: the pipeline of Item::xray (depth compare reversed, no depth write).
    vk::Pipeline PipelineFor(vk::Format color_format, vk::Format depth_format, bool xray = false,
                             const std::array<float, 4>& clip = {1.0f, 1.0f, 0.0f, 0.0f});
    bool EnsureDepth(const Target& target);
    vk::ImageView IconView(const RecorderProxy& cmdbuf, u32 icon_id);
    // The model's GPU copy (uploaded on first use); nullptr while it loads or when it is missing.
    const GpuModel* ModelOf(const RecorderProxy& cmdbuf, s32 model_id);
    void DestroyModelLater(GpuModel& model);
    // The atlas row of a label (rasterised and uploaded on first use); -1 when not ready.
    int LabelRowOf(const RecorderProxy& cmdbuf, const VR::InFrame::Label& label);
    bool Upload(const RecorderProxy& cmdbuf, vk::Image image, const void* data, size_t size,
                const vk::BufferImageCopy& region, bool first_use);
    void CollectTimes();
    // The depth probe (VR::InFrame::Probe): its texels of the game's depth copied for the CPU.
    void RecordProbe(const RecorderProxy& cmdbuf, const Target& t, const Camera& camera,
                     const VR::InFrame::List& list);
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
        std::array<float, 4> clip{};
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
    VideoCore::UniqueImage cover;
    vk::UniqueImageView cover_view;

    // Textures.
    std::unordered_map<u32, Texture> icons;
    std::unordered_map<s32, GpuModel> models;
    std::unordered_map<s32, bool> model_missing; // the game files have no such model
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
};

} // namespace Vulkan
