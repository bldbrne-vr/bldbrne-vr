// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <memory>
#include <tsl/robin_map.h>
#include "video_core/renderer_vulkan/vr_scene_proxy.h"
#include "video_core/renderer_vulkan/vr_temporal_failure.h"
#include "video_core/renderer_vulkan/vr_temporal_scene.h"
namespace Vulkan {
class GraphicsPipeline;
// Per-frame native-stereo temporal state. Accepted OFF skips scene and motion work.
// VR_TEMPORAL_NATIVE_CHECK=1 keeps full-resolution AA for diagnostic comparisons.
struct VrTemporalCapture {
    VrTemporalCapture(const Instance& i, Scheduler& s) : instance(i), scheduler(s), scene(i,s) {}
    ~VrTemporalCapture();
    // `format`: a view format other than the image's (S1 + DLSS 5: X's sRGB twin, sampled only).
    bool Views(VideoCore::Image& image, bool depth, vk::Format format = vk::Format::eUndefined);
    void RetireViews(unsigned slot);
    const Instance& instance;
    Scheduler& scheduler;
    VrTemporalScene scene;
    Motion::IndexRangeCache index_ranges;
    // Toggle bit 36: PipelineCache::GetVrMotionPipeline's non-null answers by base pipeline.
    // Only VrTemporalSelect calls it and the cache changes an entry only inside such a call, so
    // an entry here is the cache's own (see VrTemporalSelect). Cleared while the bit is off.
    struct MotionVariant {
        Shader::VrMotion motion{};
        const GraphicsPipeline* variant{};
    };
    tsl::robin_map<const GraphicsPipeline*, MotionVariant> motion_variants;
    struct CachedViews {
        vk::Image image{};
        uint64_t uid{};
        vk::Format format{};
        std::array<vk::UniqueImageView,2> eyes;
    };
    std::array<CachedViews,2> views; // colour, depth
    VideoCore::ImageId depth_id{}, color_id{};
    uint64_t depth_uid{}, color_uid{}, frame{UINT64_MAX}, accepted_key{};
    uint32_t dlss_preset{}; // the Plan's DLSS model at the last frame start (vr_dlss_preset.h)
    uint32_t width{}, height{}, input_width{}, input_height{}, draws{}, motion_draws{}, refused{}, reduced_draws{};
    std::vector<std::pair<VideoCore::ImageId,uint64_t>> proxies;
    uint32_t index_syncs{}, index_writes{}, incomplete_frames{};
    uint32_t motion_characters{}, motion_player{}, motion_camera_only{};
    bool object_candidate{}, gbuffer{}, active{}, resolved{}, reconstructed{}, faulted{};
    std::chrono::steady_clock::time_point last_resolve{};
    std::string failure;
    // S1 (FAST bit 51, vk_rasterizer_temporal.cpp): the guest renders the whole frame at the
    // input size; at the composite the provider reconstructs its target X into `color` (output
    // size, X's format, 2 layers), which our in-frame items and the UI layer's scene copy then
    // use in place of X. Recording thread only (the draw stream), like the rest of this struct.
    struct WholeFrame {
        bool on{};      // latched at the first draw: the boot patch was generated at input size
        bool pending{}; // the scene is complete (0x9a9cf8a9): reconstruct at the composite
        bool valid{};   // this frame's `color` holds the reconstruction
        uint32_t width{}, height{}; // output
        VideoCore::UniqueImage color, depth;
        // DLSS 5 only: the provider's linear eyes are blitted into this sRGB twin of X's format
        // (the blit encodes), then copied bit for bit into `color` (S1Encode).
        VideoCore::UniqueImage encode;
        vk::Format color_format{}, depth_format{}, srgb_format{}, encode_format{};
        vk::ImageLayout color_layout{vk::ImageLayout::eUndefined};
        std::array<vk::UniqueImageView, 2> srgb; // per layer, the in-frame items' targets
        VideoCore::ImageId x_id{};
        uint64_t x_uid{};
        bool depth_blit{true}; // the scene depth's format can be blitted (else no items)
        uint64_t frames{}, reconstructed{}, failed{}, missed{}, inframe{}, ui_scene{};
        // FAST_GPU_S1_ASYM (vr_temporal_failure.h): the soft latch's retry, and its counters
        // (latches, retries started, retries that reconstructed, frames waited idle;
        // FAST_GPU_S1_ASYM_VERIFY's checks of the waiting frames and their mismatches).
        TemporalRetry retry;
        uint64_t latches{}, retries{}, recovered{}, waited{}, retry_checked{}, retry_mismatches{};
        // FAST_S1_VERIFY=N (S1Verify): readback of X and the output, compared without waiting.
        std::shared_ptr<VideoCore::UniqueBuffer> verify_download;
        void* verify_mapped{};
        uint64_t verify_size{}, verify_ticket{}, verify_compared{}, verify_mismatched{};
        uint32_t verify_xw{}, verify_xh{};
        bool verify_pending{};
        double verify_same{}, verify_cross{};
        // FAST bit 59 (S1HudOutput, vk_rasterizer_vr.cpp VrUiArm): the HUD's K/W at the output
        // size; the UI passes' depth/stencil is this output-size twin of the game's, refreshed
        // by nearest blits when the game may have written it (S1UiDepth).
        VideoCore::UniqueImage ui_ds;
        vk::UniqueImageView ui_ds_view;
        vk::Format ui_ds_format{};
        vk::ImageLayout ui_ds_layout{vk::ImageLayout::eUndefined};
        uint32_t ui_ds_layers{};
        bool ui_ds_dirty{true};
        bool ui_ds_failed{};
        uint64_t hud_frames{}, hud_syncs{}, hud_draws{};
    } s1;
    vk::ImageView S1UiDepth(VideoCore::Image& ds, bool sync);
    bool S1Color(const VideoCore::Image& x);
    bool S1Depth(const VideoCore::Image& depth);
    bool S1SrgbViews(vk::Format srgb);
    bool S1Encode(vk::Format srgb);
    void S1ColorTo(vk::ImageLayout layout, vk::PipelineStageFlags2 stage, vk::AccessFlags2 access);
    void S1Verify(const VideoCore::Image& x, uint64_t frame);
    void S1Compare();
    void RetireS1();
};
} // namespace Vulkan
