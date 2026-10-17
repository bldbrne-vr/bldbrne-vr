// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/types.h"
#include "core/vr/color_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_stereo_warp_math.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

namespace VrStereo {

/// Push constants of vr_dibr_warp.comp (WarpConstants there; std430), shared by VrStereoWarp and
/// the late warp (renderer_vulkan/vr_late_warp.h), which runs the same shader built with LATE_WARP.
struct WarpPush {
    s32 crop[2][4]; ///< per eye: x, y, w, h (source pixels)
    s32 dst[2][4];
    s32 src_size[2];
    float tau;
    float reach;
    float hud_thresh;
    float gamma;
    u32 flags;
    u32 first_eye;
    float knee_px; ///< depth (and LATE_WARP's colour test): the prep's knee
    float knee_r;
    float z_a; ///< depth = z_a - z_b * D
    float z_b;
    float z_hud;
    u32 depth_base1; ///< eye 1's first float in the depth buffer
    float k_px;      ///< LATE_WARP: K, the disparity of the capture's 1/z
    /// LATE_WARP, late_warp_edge_blend (VrLate::EdgeBlendScale): < 0 off (the nearest texel at
    /// every depth edge); else the capture -> V parallax per unit of 1/z over the threshold
    float edge_k{-1.0f};
};
static_assert(sizeof(WarpPush) == 128); // the guaranteed 128 bytes of push constants

enum WarpFlag : u32 {
    FlagCatmullRom = 1u << 0,
    FlagHudDiff = 1u << 1,
    FlagHudPremul = 1u << 2,
    FlagBgra0 = 1u << 3,
    FlagBgra1 = 1u << 4,
    FlagDebugHoles = 1u << 5,
    FlagDecodeScene = 1u << 6,
    FlagDecodeHud = 1u << 7,
    FlagDebugHud = 1u << 8,
    FlagDepth = 1u << 9,
    FlagItemMask = 1u << 10,
    FlagEyeW = 1u << 11, ///< LATE_WARP: each eye pixel's surface 1/z into binding 10 (eye_taa)
    /// FlagEyeW, eye_taa_follow: the displayed view's 1/z with its history class
    /// (VrEyeTaa::DecodeEyeW) instead of the capture's
    FlagEyeWView = 1u << 12,
    /// ... and the world moved since the TAA's history (held items and the near field: none)
    FlagEyeWMoving = 1u << 13,
};

} // namespace VrStereo

/**
 * DIBR stereo (stereo_mode=2): both eyes from ONE centre-eye game frame.
 *
 * Per game frame, everything is recorded into the game's draw command stream (draw_scheduler):
 *  1. At the HUD boundary (composite VS 0x34f1f46b160), outside a render pass:
 *       Capture()        - depth -> per-eye disparity + 16x16 tile min/max (compute, prep pass)
 *                          and a copy of the pre-HUD scene colour into a VR-owned image.
 *     (or CaptureDepth() before the composite and CaptureColor() after it).
 *  2. At flip (Presenter::PrepareFrame), once the HUD has been drawn:
 *       CaptureHud()     - optional copy of the final (scene + HUD) image, or pass its view to
 *                          Warp() as ExternalHud.
 *       Warp()           - writes the left and right eye images (same instant) into the given
 *                          targets: each eye only its crop rect, at the given target offset.
 * Every game image handed in must already be in the stated layout; the caller transitions it
 * through VideoCore::Image::Transit so the texture cache's layout tracking stays correct. The
 * VR-owned images are allocated on first use and re-created only when size/format change (the
 * old ones are destroyed through Scheduler::DeferOperation once the GPU is past them).
 * Not thread-safe: capture and warp must be recorded from the same thread / command stream.
 *
 * The late warp (vr.ini late_warp, renderer_vulkan/vr_late_warp.h) re-sees a frame's capture from
 * the head at every XR frame's display time, on the frame thread, while the game draws the next
 * ones: with EnableLateRing the captures (scene colour, HUD copy, item mask, the true 1/z and its
 * tiles) rotate through LateSlots slots, a frame's capture never overwrites a slot the presenter
 * still shows or has queued (its busy mask), and GetLateCapture hands the current one over.
 */
class VrStereoWarp {
public:
    using CameraParams = VrStereo::CameraParams;

    enum class HudMode : u32 {
        None = 0,          ///< the warp shows the captured scene only
        Diff = 1,          ///< HUD = where the final image differs from the pre-HUD scene copy
        Premultiplied = 2, ///< HUD layer H (premultiplied, game's encoded domain) over the scene
    };

    enum class SourceEncoding : u32 {
        Srgb = 0,   ///< the captured images hold sRGB-encoded values (PrepareFrame's view)
        Linear = 1, ///< the captured images hold linear values
    };

    struct Settings {
        // Used by the depth capture (stored in CaptureInfo; the warp uses the captured values).
        float knee_m{0.6f};      ///< near-field compression knee, real metres (<= 0: off)
        float min_depth_m{0.3f}; ///< apparent minimum distance, real metres
        /// A capsule stamped into the disparity (the game's held vial, whose glass writes no
        /// depth); `valid` false: none.
        VrStereo::DepthProxy proxy{};
        // Used by the warp.
        float hud_depth_m{2.0f};   ///< HUD panel distance, real metres (<= 0: HUD at infinity)
        float tau_px{1.0f};        ///< neighbour disparity jump treated as a depth edge
        float hud_threshold{0.0f}; ///< HudMode::Diff: linear difference that counts as HUD
        float gamma{1.0f};         ///< output encode (post_process.frag gamma; 1.0 = sRGB)
        VR::Color::Settings color{};
        HudMode hud_mode{HudMode::Diff};
        SourceEncoding source_encoding{SourceEncoding::Srgb};
        bool catmull_rom{true};  ///< Catmull-Rom on continuous surfaces (linear when false)
        bool debug_holes{false}; ///< paint disocclusion holes green
        bool debug_hud{false};   ///< paint the HUD mask magenta (HudMode::Diff)
        /// Also write each eye pixel's depth into DepthBuffer() (0..1 between DepthNearM and
        /// DepthFarM, true depth: the near-field knee undone; the HUD at hud_depth_m).
        bool depth{false};
        float depth_debug_m{0.0f}; ///< > 0: every pixel at this distance (proof test)
    };

    /// The fixed planes of the depth the warp writes (Settings::depth): a standard, not reversed,
    /// perspective depth, d = (1/near - 1/z) / (1/near - 1/far), z in metres.
    static constexpr float DepthNearM = 0.05f;
    static constexpr float DepthFarM = 10000.0f;

    /// Everything Capture() needs, recorded at the composite draw.
    struct CaptureInputs {
        vk::Image color_image{}; ///< composite colour target X (pre-HUD)
        vk::ImageLayout color_layout{vk::ImageLayout::eTransferSrcOptimal}; ///< or eGeneral
        vk::Format color_format{}; ///< X's real VkFormat (backing->image.image_ci.format)
        vk::Extent2D color_extent{};
        vk::SampleCountFlagBits color_samples{vk::SampleCountFlagBits::e1}; ///< >1: resolved
        vk::ImageView depth_view{}; ///< depth-ASPECT sampled view (or an R32F copy), 1 sample
        vk::ImageLayout depth_layout{vk::ImageLayout::eDepthStencilReadOnlyOptimal};
        vk::Extent2D depth_extent{};
        CameraParams camera{}; ///< the camera that rendered this frame (CameraRecord)
    };

    /// One eye's output target.
    struct EyeTarget {
        vk::Image image{};            ///< for the layout barriers
        vk::ImageView storage_view{}; ///< R8G8B8A8_UNORM view, image has STORAGE usage
        vk::Extent2D target_extent{}; ///< size of `image`; the written rect is clipped to it
        vk::Rect2D src_rect{};        ///< this eye's crop in render pixels (CropToEyes rect)
        vk::Offset2D dst_offset{};    ///< where src_rect.offset lands in the target
        vk::ImageLayout old_layout{vk::ImageLayout::eUndefined}; ///< contents are overwritten
        vk::ImageLayout new_layout{vk::ImageLayout::eTransferSrcOptimal};
        bool bgra{true}; ///< store B,G,R,A bytes (bit copy into a B8G8R8A8 swapchain)
    };

    /// Optional HUD source that is not owned by the warp (e.g. X's view at PrepareFrame).
    struct ExternalHud {
        vk::ImageView view{};     ///< same size as the captured scene, eShaderReadOnlyOptimal
        bool needs_decode{false}; ///< UNORM view holding sRGB values (no *_SRGB view used)
    };

    /// Crop-sized eye images for one presenter frame (R8G8B8A8_UNORM, storage + transfer src).
    struct EyeImages {
        EyeImages() = default; // UniqueImage's default constructor is explicit: no aggregate init

        std::array<VideoCore::UniqueImage, 2> image;
        std::array<vk::UniqueImageView, 2> view;
        std::array<vk::Extent2D, 2> extent{};
        std::array<vk::ImageLayout, 2> layout{vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eUndefined};
    };

    /// The late warp's source: one frame's capture in its slot (valid until the slot is written
    /// again: the presenter keeps the slots it uses in the busy mask given to EnableLateRing).
    struct LateCapture {
        bool valid{false};
        u32 slot{0};
        vk::Extent2D size{};
        vk::ImageView scene{};         ///< colour (linear values unless scene_decode)
        bool scene_decode{false};      ///< the shader decodes sRGB (FlagDecodeScene)
        vk::ImageView inverse_depth{}; ///< R32F 1/z in game units (0 = sky)
        vk::ImageView tiles{};         ///< RGBA32F per 16x16 tile: .b / .a = min / max 1/z
        vk::ImageView hud{};           ///< HudMode::Diff: the final image (null: no HUD)
        bool hud_decode{false};
        vk::ImageView item_mask{}; ///< in-frame items' coverage (null: none)
        CameraParams camera{};
        VrStereo::DisparityParams disparity{};
        float reach_px{};
        Settings settings{}; ///< the warp settings the frame's eyes were made with
    };
    static constexpr u32 LateSlots = 5; ///< at most (OpenXRPresenter::LateSlotsWanted)

    /// What the current capture holds. The presenter submits the eye poses with the same
    /// baseline: left = head - right * VrStereo::EyeOffsetMetres(camera), right = head + ...
    struct CaptureInfo {
        u64 seq{};       ///< increments on every complete (colour + depth) capture
        u64 frame_tag{}; ///< tag passed to BeginFrame()
        vk::Extent2D size{};
        vk::Format color_format{};
        CameraParams camera{};
        VrStereo::DisparityParams disparity{};
        VrStereo::ProxyParams proxy{}; ///< the depth proxy as stamped (on = 0: none)
        float reach_px{};
        bool color_valid{};
        bool depth_valid{};
        bool hud_valid{};
        bool mask_valid{}; ///< CaptureItemMask this frame
    };

    explicit VrStereoWarp(const Instance& instance, Scheduler& scheduler);
    ~VrStereoWarp();

    VrStereoWarp(const VrStereoWarp&) = delete;
    VrStereoWarp& operator=(const VrStereoWarp&) = delete;

    /// Starts a new game frame: the previous frame's capture can no longer be warped.
    void BeginFrame(u64 frame_tag);

    /// CaptureDepth + CaptureColor in one call (when the composite does not write depth).
    bool Capture(vk::CommandBuffer cmdbuf, const CaptureInputs& in, const Settings& settings);

    /// Depth -> disparity (prep compute). scene_extent = the composite target's size (the
    /// disparity image matches the colour capture). Must be recorded outside a render pass.
    /// aa_mode=taa (renderer_vulkan/vr_render_taa.h): the resolve's stable depth (R32F, the
    /// depth's read size, eGeneral) stands in for the game's jittered depth `raw`; where
    /// depth_view (our in-frame items' private copy) is nearer than `raw`, our items stay.
    struct StableDepthInput {
        vk::ImageView stable{};
        vk::ImageView raw{};
        vk::ImageLayout raw_layout{};
    };

    bool CaptureDepth(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                      vk::ImageLayout depth_layout, vk::Extent2D depth_extent,
                      vk::Extent2D scene_extent, const CameraParams& camera,
                      const Settings& settings, const StableDepthInput* stable = nullptr);

    /// Copies (or resolves) the pre-HUD scene colour. src must be in eTransferSrcOptimal or
    /// eGeneral. `components`: how the true R, G, B, A are read from the image's channels (a
    /// colour buffer written with a component swap, e.g. BGRA bytes in an R8G8B8A8 image).
    /// Must be recorded outside a render pass.
    bool CaptureColor(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout,
                      vk::Format format, vk::Extent2D extent,
                      vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1,
                      const vk::ComponentMapping& components = {});

    /// Copies the final image (scene + HUD, same format and size as the scene capture) for
    /// HudMode::Diff, or the premultiplied HUD layer for HudMode::Premultiplied.
    bool CaptureHud(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout,
                    vk::Format format, vk::Extent2D extent,
                    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1,
                    const vk::ComponentMapping& components = {});

    /// Our in-frame items' coverage (R8 UNORM, src in eTransferSrcOptimal, the capture's size;
    /// renderer_vulkan/vr_inframe.h), copied after CaptureColor. With HudMode::Diff the HUD over
    /// those pixels is applied to the warped item (darkening as a ratio, bright marks as HUD)
    /// instead of replacing them at the HUD's disparity, which would cut a near item along the
    /// edge of a HUD element. Without a call this frame, the warp is as before.
    bool CaptureItemMask(vk::CommandBuffer cmdbuf, vk::Image src, vk::Extent2D extent);

    /// True when this frame has a colour + depth capture of matching size.
    [[nodiscard]] bool HasCapture() const;

    /// The pre-HUD scene copy (the UI layer veils it in place and makes the mono eye from it).
    struct SceneImage {
        vk::Image image{};
        vk::ImageView raw{};     ///< raw format, the capture's component mapping
        vk::ImageView decoded{}; ///< sRGB twin (hardware decode), or null
        vk::ImageView storage{}; ///< raw format, identity mapping, or null (no storage support)
        vk::Extent2D extent{};
        vk::Format format{};
    };
    /// Valid while this frame's colour capture is (eShaderReadOnlyOptimal).
    [[nodiscard]] SceneImage Scene() const;

    [[nodiscard]] const CaptureInfo& GetCaptureInfo() const {
        return info;
    }

    /// The late warp (before the first capture): captures rotate through `count` slots (at most
    /// LateSlots) and also keep the true 1/z; a slot whose bit is set in `busy` (written by the
    /// presenter, any thread) is never written.
    void EnableLateRing(const std::atomic<u32>* busy, u32 count = 3);
    [[nodiscard]] bool LateRingEnabled() const {
        return slot_count > 1;
    }
    /// After Warp() (same thread): the current capture for the late warp, with the settings its
    /// eyes were warped with (invalid without a capture or without the ring).
    [[nodiscard]] LateCapture GetLateCapture(const Settings& settings) const;

    /// Records the warp of both eyes (nullptr skips that eye; at least one must be given).
    /// eyes[0] = left, eyes[1] = right. Returns false (nothing recorded) without a capture.
    /// The HUD source is `hud` when it has a view, else the CaptureHud() image, else none.
    /// Must be recorded outside a render pass.
    bool Warp(vk::CommandBuffer cmdbuf, const std::array<const EyeTarget*, 2>& eyes,
              const Settings& settings, const ExternalHud& hud);
    bool Warp(vk::CommandBuffer cmdbuf, const std::array<const EyeTarget*, 2>& eyes,
              const Settings& settings);

    /// Convenience: (re)creates crop-sized eye images in `images`, warps into them at offset 0
    /// and leaves them in final_layout (layouts tracked in `images`). The short overload uses
    /// eTransferSrcOptimal, BGRA byte order and no external HUD.
    bool WarpToEyeImages(vk::CommandBuffer cmdbuf, EyeImages& images,
                         const std::array<vk::Rect2D, 2>& src_rects, const Settings& settings,
                         vk::ImageLayout final_layout, bool bgra, const ExternalHud& hud);
    bool WarpToEyeImages(vk::CommandBuffer cmdbuf, EyeImages& images,
                         const std::array<vk::Rect2D, 2>& src_rects, const Settings& settings);

    /// (Re)creates `images` with the given per-eye extents (no-op when unchanged).
    void EnsureEyeImages(EyeImages& images, vk::Extent2D left, vk::Extent2D right);

    /// The depth the last Warp() wrote with Settings::depth: one float per eye pixel, rows of the
    /// eye's crop width, eye `e` from DepthOffset(e) bytes. Written in the draw stream; read it in
    /// the same command stream after the warp (it is overwritten by the next frame's).
    [[nodiscard]] vk::Buffer DepthBuffer() const {
        return depth_buffer;
    }
    [[nodiscard]] vk::DeviceSize DepthOffset(u32 eye) const {
        return depth_offset[eye];
    }

    /// GPU timestamps around the prep pass (CaptureDepth) and around the HUD copy + warp
    /// (CaptureHud .. Warp) of every frame. Results are read a few frames later, never waited
    /// for. Each span starts after all earlier commands finished (an upper bound of the cost).
    void EnableTiming(const Instance& instance);
    struct GpuTimes {
        u32 frames{};    ///< frames with both spans measured
        float prep_ms{}; ///< averages
        float warp_ms{};
        float prep_max_ms{};
        float warp_max_ms{};
    };
    /// Times measured since the last call (frames = 0 when timing is off).
    GpuTimes TakeGpuTimes();

private:
    struct OwnedImage {
        OwnedImage() = default;

        VideoCore::UniqueImage image;
        vk::UniqueImageView raw_view;  ///< view in the image's own format (also storage view)
        vk::UniqueImageView srgb_view; ///< *_SRGB twin view (hardware decode), when one exists
        /// Identity-mapped raw view for storage, when the image has storage usage (the scene: the
        /// UI layer's in-place veil).
        vk::UniqueImageView storage_view;
        vk::Extent2D extent{};
        vk::Format format{vk::Format::eUndefined};
        vk::ComponentMapping components{}; ///< of the sampled views

        /// View that yields linear values for sRGB-encoded content (twin view or raw view).
        vk::ImageView DecodedView() const {
            return srgb_view ? *srgb_view : *raw_view;
        }
        /// True when DecodedView() decodes in hardware (else the shader must decode).
        bool HwDecode() const {
            return static_cast<bool>(srgb_view);
        }
    };

    /// One capture: what a frame's warp (and the late warp after it) reads.
    struct CaptureSlot {
        OwnedImage scene;         ///< pre-HUD centre colour (copy of X)
        OwnedImage hud;           ///< final image or HUD layer
        OwnedImage tiles;         ///< RGBA32F 16x16 (min D, max D, min 1/z, max 1/z)
        OwnedImage item_mask;     ///< R8 our in-frame items' coverage (CaptureItemMask)
        OwnedImage inverse_depth; ///< R32F 1/z (the late ring only)
    };
    CaptureSlot& Slot() {
        return slots[slot_index];
    }
    const CaptureSlot& Slot() const {
        return slots[slot_index];
    }
    /// A new capture starts (BeginFrame with a tag): the next slot nobody uses.
    void NextSlot();

    void EnsureImage(OwnedImage& img, vk::Extent2D extent, vk::Format format,
                     vk::ImageUsageFlags usage, bool srgb_views, const char* name,
                     const vk::ComponentMapping& components = {});
    void RetireImage(OwnedImage& img);
    void CopyInto(vk::CommandBuffer cmdbuf, OwnedImage& dst, vk::Image src,
                  vk::ImageLayout src_layout, vk::Extent2D extent,
                  vk::SampleCountFlagBits samples) const;
    void Timestamp(vk::CommandBuffer cmdbuf, u32 query);
    void CollectTimes();

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device{};
    VmaAllocator allocator{};

    vk::UniqueDescriptorSetLayout prep_set_layout{};
    vk::UniquePipelineLayout prep_layout{};
    vk::UniquePipeline prep_pipeline{};
    vk::UniqueDescriptorSetLayout warp_set_layout{};
    vk::UniquePipelineLayout warp_layout{};
    vk::UniquePipeline warp_pipeline{};
    vk::UniquePipeline warp_neutral_pipeline{};

    void EnsureDepthBuffer(vk::DeviceSize size);

    vk::Buffer depth_buffer{}; ///< per-eye depth (Settings::depth)
    VmaAllocation depth_allocation{};
    vk::DeviceSize depth_capacity{};
    std::array<vk::DeviceSize, 2> depth_offset{};

    OwnedImage disparity; ///< R32F |D| px (one: read by the frame's own warp only)
    std::array<CaptureSlot, LateSlots> slots{};
    u32 slot_index{0};
    u32 slot_count{1}; ///< EnableLateRing's count with the late ring
    const std::atomic<u32>* late_busy{nullptr};
    std::array<u64, LateSlots> slot_written{}; ///< the capture count when each slot was last used
    u64 captures{0};

    CaptureInfo info{};

    // Timing (EnableTiming): TimingSlots frames in flight, 4 queries each (prep start/end, warp
    // start/end).
    static constexpr u32 TimingSlots = 4;
    vk::UniqueQueryPool query_pool{};
    float timestamp_period_ns{1.0f};
    u32 timing_slot{0};
    u32 timing_written{0}; ///< bit mask of this frame's written queries
    std::array<bool, TimingSlots> timing_pending{};
    GpuTimes timing_sum{};
};

} // namespace Vulkan
