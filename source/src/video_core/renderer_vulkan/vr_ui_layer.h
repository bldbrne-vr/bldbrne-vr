// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
class RecorderProxy;

/**
 * The game's UI on its own compositor layer (vr.ini ui_layer, documents/bloodborne-vr.md).
 *
 * Every draw after the game's composite (VR::Settings::hud_after_vs) is UI. In a UI-layer frame
 * the rasterizer draws each UI colour draw into the composite target X three times: into the
 * VR-owned target W (cleared to grey g), into K (cleared to black), both with the depth and
 * stencil writes masked, and the game's own draw into X, unchanged. For every blend that is
 * affine in the destination colour, X = K + T * S per channel, with S the scene before the UI,
 * emission K and transmittance T = (W - K) / g: the UI alone, exactly, with nothing changed in
 * the game's own image (the desktop mirror keeps the UI). The UI draws are native size in these
 * frames (no HUD panel shrink), the eyes are made from the scene copy taken at the composite, and
 * the presenter shows the UI as transparent quads and cylinders over the live stereo world.
 *
 * Everything is recorded into the game's draw stream (the draw scheduler): this frame's K/W
 * passes, the resolve at its flip (Presenter::PrepareFrame) and the next frame's clears are
 * ordered by plain barriers, so one K/W pair serves every frame.
 *
 * The lock-on marker (ui_lock_marker_scale / ui_lock_marker_alpha, VR::Ui::LockMarker) is drawn
 * into a pair of its own (MarkBlack, MarkGrey) instead of K and W; the resolve measures its box,
 * scales it about the box's centre, dims it and puts it under the rest of the UI.
 */
class VrUiLayer {
public:
    enum Which : u32 { Black = 0, Grey = 1, MarkBlack = 2, MarkGrey = 3 }; // K, W, Km, Wm

    /// Pre-UI scene at render size: one layer for mono/AER, two for native stereo.
    struct SceneRef {
        vk::Image image{};
        vk::ImageView sampled{}; ///< raw format, X's component mapping (encoded values)
        vk::ImageView storage{}; ///< raw format, identity mapping (the in-place veil); may be null
        vk::ImageView decoded{}; ///< sRGB twin with X's component mapping; may be null
        vk::Extent2D extent{};
        /// Its layout outside the resolve (the veil returns it there).
        vk::ImageLayout layout{vk::ImageLayout::eShaderReadOnlyOptimal};
        u32 layers{1}; ///< native stereo preserves both pre-UI scene layers
        vk::ImageView right_storage{}; ///< the right scene receives the same menu dim
    };

    struct Region {
        s32 x0{}, y0{}, x1{}, y1{}; ///< band pixels, end exclusive
    };

    struct ResolveParams {
        vk::Rect2D band{};           ///< the 16:9 band of the render the layer shows
        bool bgra{true};             ///< atlas bytes for a B8G8R8A8 swapchain
        bool veil{true};             ///< take the game's uniform black dim out (into the eyes)
        std::array<Region, 4> cut{}; ///< HUD band regions (left out of the rest half)
        u32 cut_count{};
        u32 status_cuts{}; ///< bit i: cut i is left out of the content box (the HUD clusters)
        /// The cut split by content (VR::Ui::HudSplit) in a gameplay frame, -1 = none: its wide
        /// elements stay on the band, the rest of it on the rest half.
        s32 dynamic_cut{-1};
        bool dialogue{false}; ///< entire subtitle region stays at one stereo depth
        bool item_pickup{false}; ///< keep the pickup confirmation on the lower HUD from onset
        bool action_prompt{false}; ///< route the prompt to the HUD even during its faint first frame
        /// An area name is up (VR::Ui::StepAreaName): the dynamic cut reaches the stage's bottom,
        /// the name's corner goes to the band whole, its dark backing keyed out.
        bool area_name{false};
        /// ... and its dark backing is keyed out (the HUD movie still has its dark strip:
        /// VR::IsAreaDimRemoved false); else the corner is left as the game draws it.
        bool area_key{false};
        bool gameplay{false}; ///< no game menu: the dynamic cut is split (else it stays whole)
        /// A full-screen menu on the canvas: a frame without it (UI only in the status cuts, the
        /// menu state a few frames off the drawing) leaves the canvas empty.
        bool full_menu{false};
        /// A gameplay frame: the status cuts' UI footprint (16 x 16 px cells) is kept for the
        /// menus (vr_ui_resolve.comp FOOTPRINT_BASE).
        bool footprint{false};
        /// A full menu cropped to its content (ui_menu_fit): its content box is measured in `box`
        /// with only the gameplay HUD's footprint left out of the status cuts (not the cuts).
        bool menu_box{false};
        /// A full menu's frame (ui_menu_hud): the HUD the game drew under the menu (K and W as
        /// they were before the menu's first draw, SnapshotHud), in the gameplay HUD's footprint
        /// cells of the status cuts, goes into the bottom half's status cuts (shown under the
        /// canvas).
        bool menu_hud{false};
        /// The split's thresholds in band px: min run width, gap bridged, rows dilated, margin.
        std::array<u32, 4> split_px{};
        Region box{};       ///< the content box is measured in this band rect
        float gamma{1.0f};  ///< the eyes' post-process gamma (the atlas gets the same curve)
        bool check{false};  ///< ui_layer=2: compare X with K + T * S (no atlas)
        SceneRef x_final{}; ///< check: X after the UI (sampled view, X's mapping)
        u64 seq{};          ///< the frame (FrameTag::seq) and its game menu class (VR::UiClass):
        u8 cls{};           ///< the content box read back later carries them
        /// The lock-on marker's factors (VR::Ui::LockMarker), for a frame that drew it into its
        /// own pair (MarkerWritten).
        float marker_scale{1.0f};
        float marker_alpha{1.0f};
        /// No UI was drawn after the marker this frame: it goes over the rest (else under it).
        bool marker_over{true};
        /// A pixel no UI drew on (K and W at their clear values to within one 8-bit step) is
        /// exactly transparent (vr.ini ui_empty_snap, VR::Ui::Empty); false: the grey's
        /// rounding leaves a black alpha of 2/255 there (the builds before 2026-10-09).
        bool empty_snap{true};
        /// The atlas has the build-up meter's strip below its halves (AtlasFor `strip`,
        /// VR::Ui::Conditions::StripTop) ...
        bool strip{false};
        /// ... and cut 3 is the meter's rows this frame (ui_conditions=view): its pixels go into
        /// the strip and leave both halves (vr_ui_resolve.comp FLAG_CONDITIONS). Needs `strip`
        /// and cut_count 4.
        bool conditions{false};
    };

    /// The bounding box of the UI in the dialog region, or a full menu's (band px, end
    /// exclusive).
    struct ContentBox {
        bool valid{false};
        s32 x0{}, y0{}, x1{}, y1{};
        /// Which parts hold any UI: bits 0..3 the cut-outs (the HUD band's segments), bit 4 the
        /// rest half.
        u32 regions{};
        /// Each cut-out's own content box (band px x0, y0, x1, y1, end exclusive; valid where
        /// `regions` has its bit): where the HUD band's pieces show something.
        std::array<std::array<s32, 4>, 4> cut_box{};
        /// Band pixels within 8 px of the band's border holding UI (VR::Ui::StepScreenWide).
        u32 border{};
        /// The right end of the dynamic cut's HUD elements (band px, end exclusive; 0: none, or
        /// not a split gameplay frame): an area name (VR::Ui::StepAreaName).
        s32 hud_right{};
        /// Band pixels holding UI (alpha > 4/255) and opaque UI (alpha > 250/255), after a
        /// uniform veil was taken out: an opaque page vs a dim (VR::Ui::IsScreenWidePage).
        u32 content{};
        u32 opaque{};
        /// The box of the opaque UI alone (band px, end exclusive; all 0: none).
        std::array<s32, 4> opaque_box{};
        u64 seq{}; ///< the frame it was measured in (ResolveParams::seq, cls)
        u8 cls{};
    };

    /// Read back from the GPU a few frames late (TakeStats).
    struct Stats {
        u32 frames{};         ///< resolved frames summed here
        u32 veil_frames{};    ///< ... with a veil taken out
        float veil_min{1.0f}; ///< the darkest veil
        float veil_last{1.0f};
        float strip_t{1.0f}; ///< the last frame's edge-strip transmittance (mean, spread)
        float strip_spread{};
        float strip_k{};
        float strip_k_spread{};
        std::array<float, 3> veil_t{1.0f, 1.0f, 1.0f}; ///< the last veil (per channel, encoded)
        std::array<float, 3> veil_k{};
        u64 content{}; ///< band pixels with alpha > 4/255 (sums)
        u64 rest{};    ///< ... of them in the rest half
        u64 border{};  ///< ... within 8 px of the band's border
        u64 opaque{};  ///< band pixels with alpha > 250/255
        std::array<u64, 4> cut{};
        u64 check_n{};    ///< check: pixels compared
        u64 check_bad{};  ///< ... more than 3 LSB off
        u64 check_bad8{}; ///< ... more than 8 LSB off
        u64 check_sum{};  ///< ... sum of the errors (LSB)
        u32 check_max{};
        u32 last_content{}; ///< the newest frame's content count
        u64 saturated{};    ///< band pixels where W clipped (additive glows, T taken as 1)
        u64 split_frames{}; ///< gameplay frames with a split dynamic region
        u64 split_rows{};   ///< ... its rows holding a HUD element
        u64 split_out{};    ///< ... its UI pixels left on the rest quad (not HUD)
        u32 menu_gone{};    ///< full menu frames drawn without the menu (canvas left empty)
        /// ResolveParams::menu_hud frames by where their HUD came from: K and W just before the
        /// menu's first draw (kept), the whole frame's (no draw of the menu: whole), none (the
        /// menu drew first: empty); the HUD px put under the canvas and the clusters' UI px left
        /// out (outside the footprint).
        u32 menu_hud_kept{};
        u32 menu_hud_whole{};
        u32 menu_hud_empty{};
        u64 menu_hud_px{};
        u64 menu_hud_out{};
        u32 menu_hud_veiled{}; ///< ... whose kept K and W held the menu's dim (taken back out)
        /// Frames whose lock-on marker was drawn into its own pair (scaled and dimmed), the
        /// frames among them with no marker pixel, and the newest one's box (band px, inclusive).
        u32 marker_frames{};
        u32 marker_empty{};
        std::array<s32, 4> marker_box{};
        float marker_scale{1.0f};
        float marker_alpha{1.0f};
    };

    /// Where this frame's HUD for a menu comes from (SnapshotHud).
    enum class HudSnap : u8 {
        Whole, ///< no draw left the HUD this frame: K and W at the resolve are the HUD alone
        Kept,  ///< K and W were kept before the first draw that is not the HUD's
        Empty, ///< that draw came before any UI draw: no HUD
    };

    struct GpuTimes {
        u32 frames{};
        float ui_ms{};      ///< the composite to the flip (the game's UI draws plus our shadows)
        float resolve_ms{}; ///< the resolve (atlas + veil)
        float ui_max_ms{};
        float resolve_max_ms{};
    };

    explicit VrUiLayer(const Instance& instance, Scheduler& scheduler);
    ~VrUiLayer();

    VrUiLayer(const VrUiLayer&) = delete;
    VrUiLayer& operator=(const VrUiLayer&) = delete;

    // ---- GPU thread, the rasterizer ----

    /// The composite of a UI-layer frame: K and W in X's format, size and component mapping
    /// (re-created only when one changes). Native stereo uses two attachment layers to match
    /// the guest multiview/stencil pass; the common UI in layer zero feeds one atlas.
    /// False: nothing (an unsupported X).
    bool BeginFrame(vk::Format x_format, vk::Extent2D extent,
                    const vk::ComponentMapping& components, float grey, u32 layers = 1);
    /// The render-target view of K or W in `view_format` (the shadowed pipeline's attachment
    /// format); the first use in a frame records its layout barrier. Outside a render pass.
    vk::ImageView TargetView(RecorderProxy cmdbuf, Which which, vk::Format view_format);
    /// FAST_GPU_PASS_MERGE (vk_pass_merge.h): K or W is in its attachment layout this frame
    /// already, so TargetView records no command for it (and may be asked inside a render pass).
    [[nodiscard]] bool TargetBound(Which which) const {
        const auto& target = targets[which];
        return frame_active && target.image &&
               target.layout == vk::ImageLayout::eColorAttachmentOptimal;
    }
    /// True once per frame and target: that pass clears it (`bits`: the clear colour).
    bool TakeClear(Which which, std::array<u32, 4>& bits);
    /// K's or W's clear colour (black / the grey, alpha 0) in its raw channel order.
    [[nodiscard]] vk::ClearColorValue ClearColor(Which which) const;
    /// The game cleared X in this pass: K and W take its clear (no own clear any more).
    void NoteGameClear(Which which);
    /// Copies X at the composite: the scene the eyes are made from (AER, mono, a refused DIBR
    /// capture, or both native-stereo eyes) and the check's reference. Outside a render pass; X in `layout`.
    bool CaptureScene(const RecorderProxy& cmdbuf, vk::Image x, vk::ImageLayout layout,
                      vk::Format format, vk::Extent2D extent,
                      const vk::ComponentMapping& components, u32 layers = 1);
    [[nodiscard]] bool HasScene() const {
        return scene_valid;
    }
    [[nodiscard]] SceneRef Scene() const;
    [[nodiscard]] std::array<vk::ImageView, 2> NativeSceneViews(vk::Format format);
    [[nodiscard]] bool FrameActive() const {
        return frame_active;
    }
    /// A K/W pass was recorded this frame.
    [[nodiscard]] bool Written() const {
        return written;
    }
    /// The lock-on marker was drawn into its own pair this frame (MarkBlack, MarkGrey).
    [[nodiscard]] bool MarkerWritten() const {
        return marker_written;
    }
    /// The game draws its HUD before a menu over it: right before the frame's first UI draw that
    /// is not the HUD's (the rasterizer's learned HUD draws), the rows `rows` (render px; the
    /// band's top, where the status clusters are) of K and W are kept for the resolve's menu HUD
    /// (ResolveParams::menu_hud). Once per frame; outside a render pass.
    void SnapshotHud(vk::CommandBuffer cmdbuf, const vk::Rect2D& rows);
    [[nodiscard]] HudSnap HudSnapState() const {
        return hud_snap;
    }
    /// The frame is over (PrepareFrame, PrepareBlankFrame): nothing of it is used any more.
    void EndFrame();
    /// VR_UI_TIMING: the GPU time from here (the composite) to the frame's resolve. Outside a
    /// render pass.
    void MarkComposite(vk::CommandBuffer cmdbuf);
    /// VR_UI_TIMING: the flip of a frame without a resolve (the UI span of today's path).
    void MarkFlip(vk::CommandBuffer cmdbuf);

    // ---- The presenter (PrepareFrame, the same stream) ----

    /// The atlas of presentation frame `slot`: band width x twice the band height (and the
    /// build-up meter's strip below with `strip`: VR::Ui::Conditions::AtlasHeight), R8G8B8A8,
    /// storage + transfer source. Left in eTransferSrcOptimal by Resolve.
    struct Atlas {
        vk::Image image{};
        vk::ImageView storage{};
        vk::Extent2D extent{};
    };
    Atlas AtlasFor(u32 slot, vk::Extent2D band, bool strip = false);
    /// The mono eye image of presentation frame `slot` (the scene without the UI, post-processed
    /// like the frame itself): `format`, colour attachment + sampled + transfer source.
    struct EyeImage {
        vk::Image image{};
        vk::ImageView view{};
        vk::Extent2D extent{};
    };
    EyeImage EyeImageFor(u32 slot, vk::Format format, vk::Extent2D extent);

    /// Records the resolve of this frame's K/W: the veil estimate, the atlas (unless check) and
    /// the veil of `scene` (in place, when it has a storage view), or the check. False: nothing.
    bool Resolve(vk::CommandBuffer cmdbuf, const Atlas* atlas, const SceneRef& scene,
                 const ResolveParams& params);

    /// Statistics of the frames resolved since the last call whose GPU work is done.
    Stats TakeStats();
    /// The content box of the newest resolved frame the CPU has seen (a few frames late).
    [[nodiscard]] ContentBox LatestContentBox() {
        CollectStats();
        return latest_box;
    }
    void EnableTiming(const Instance& instance);
    GpuTimes TakeGpuTimes();

private:
    struct Target {
        Target() = default;
        VideoCore::UniqueImage image;
        vk::UniqueImageView sampled;
        std::vector<std::pair<vk::Format, vk::UniqueImageView>> rt_views;
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
        bool cleared{false};
    };
    struct OwnedImage {
        OwnedImage() = default;
        VideoCore::UniqueImage image;
        vk::UniqueImageView sampled;
        vk::UniqueImageView storage;
        vk::UniqueImageView decoded;
        vk::UniqueImageView right_storage;
        std::vector<std::pair<vk::Format, std::array<vk::UniqueImageView, 2>>> native_views;
        u32 layers{1};
        vk::Extent2D extent{};
        vk::Format format{vk::Format::eUndefined};
        vk::ComponentMapping components{};
    };

    void Retire(VideoCore::UniqueImage& image, std::vector<vk::UniqueImageView> views);
    void EnsureTargets(vk::Format format, vk::Extent2D extent, const vk::ComponentMapping& comps,
                       u32 layers);
    void CollectStats();
    void Timestamp(vk::CommandBuffer cmdbuf, u32 query);
    void CollectTimes();

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device{};
    VmaAllocator allocator{};

    std::array<Target, 4> targets; ///< K, W and the lock-on marker's pair (Which)
    /// K and W kept before a menu's first draw (SnapshotHud): K's and W's format, size and
    /// mapping, created with them.
    struct HudKeep {
        HudKeep() = default;
        VideoCore::UniqueImage image;
        vk::UniqueImageView sampled;
    };
    std::array<HudKeep, 2> hud_keep;
    HudSnap hud_snap{HudSnap::Whole};
    vk::Format format{vk::Format::eUndefined};
    vk::Extent2D extent{};
    vk::ComponentMapping components{};
    float grey{0.5f};
    u32 target_layers{1};
    bool frame_active{false};
    bool written{false};
    bool marker_written{false};
    bool srgb_rt{false};

    OwnedImage scene;
    bool scene_valid{false};

    struct AtlasImage {
        AtlasImage() = default;
        VideoCore::UniqueImage image;
        vk::UniqueImageView storage;
        vk::Extent2D extent{};
    };
    std::vector<AtlasImage> atlases;
    struct EyeImageOwned {
        EyeImageOwned() = default;
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Extent2D extent{};
        vk::Format format{vk::Format::eUndefined};
    };
    std::vector<EyeImageOwned> eye_images;

    vk::UniqueDescriptorSetLayout set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    /// veil estimate, atlas, scene veil, check, split rows, split, menu gone, menu HUD, marker box
    std::array<vk::UniquePipeline, 9> pipelines{};

    // Stats: the device-side words the passes write, copied per frame into a host-visible ring.
    static constexpr u32 StatsWords = 64;
    static constexpr u32 ReadbackSlots = 8;
    vk::Buffer stats_buffer{};
    VmaAllocation stats_allocation{};
    vk::Buffer readback_buffer{};
    VmaAllocation readback_allocation{};
    const u32* readback_words{};
    std::array<u64, ReadbackSlots> readback_tick{};
    std::array<bool, ReadbackSlots> readback_pending{};
    std::array<u64, ReadbackSlots> readback_seq{}; ///< ResolveParams::seq, cls, gameplay split
    std::array<u8, ReadbackSlots> readback_cls{};
    std::array<bool, ReadbackSlots> readback_split{};
    std::array<u8, ReadbackSlots> readback_hud{}; ///< 0: no menu HUD, else 1 + its HudSnap
    std::array<bool, ReadbackSlots> readback_marker{}; ///< the lock-on marker was resolved
    std::array<float, ReadbackSlots> readback_marker_scale{};
    std::array<float, ReadbackSlots> readback_marker_alpha{};
    // VR_UI_LOG: the content of the frames around a change of the game menu class (menu
    // detection against what the frames draw).
    struct ClassTrace {
        u64 seq{};
        u8 cls{};
        u32 content{};
        u32 rest{};
    };
    std::array<ClassTrace, 4> class_trace{};
    u32 class_trace_count{0};
    u32 class_trace_after{0};
    // The dynamic region's split (vr_ui_resolve.comp binding 7): per row its HUD extent, then
    // the UI bit mask; cleared once, then by the split pass itself.
    static constexpr u32 SplitBytes = 4u << 20;
    vk::Buffer split_buffer{};
    VmaAllocation split_allocation{};
    bool split_cleared{false};
    u32 readback_slot{0};
    Stats stats_sum{};
    ContentBox latest_box{};
    u64 box_tick{0};

    // Timing (VR_UI_TIMING): per frame the composite, the resolve's start and end.
    static constexpr u32 TimingSlots = 4;
    vk::UniqueQueryPool query_pool{};
    float timestamp_period_ns{1.0f};
    u32 timing_slot{0};
    u32 timing_written{0};
    std::array<bool, TimingSlots> timing_pending{};
    GpuTimes timing_sum{};
};

} // namespace Vulkan
