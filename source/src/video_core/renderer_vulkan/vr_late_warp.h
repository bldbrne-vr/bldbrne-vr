// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_OPENXR

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <openxr/openxr.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_eye_taa_math.h"
#include "video_core/renderer_vulkan/vr_eye_upscale.h"
#include "video_core/renderer_vulkan/vr_late_warp_math.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"
#include "video_core/renderer_vulkan/vr_vignette_warp.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// The late warp (vr.ini late_warp): every XR frame, the DIBR capture of the game frame on display
// (centre colour + depth + its camera) is warped again to the eyes of the head at that XR frame's
// predicted display time, so head rotation AND translation are smooth at the headset's rate
// whatever the game's frame rate. Two compute passes (vr_late_warp_math.h has the model):
//   1. vr_late_reproject.comp: the capture S re-seen from V (the head at display time, S's
//      intrinsics): per V pixel its place in S (an epipolar scan of S's 1/z), V's disparity and
//      tiles;
//   2. vr_dibr_warp.comp built with LATE_WARP: both eyes from V exactly as the game-rate DIBR warp
//      makes them from S (the same near-field knee, edges, holes, Catmull-Rom, HUD), each colour
//      looked up once in S through pass 1's map; into one double-wide image, copied into this
//      renderer's own double-wide OpenXR swapchain.
// late_warp_motion (Source::motion): the world also moved on with the game camera since the
// capture (walking, turning): once per capture vr_late_classify.comp sorts its texels into the
// hunter's body (Source::body capsules) and the world, and pass 1 scans the body through S as
// before and the world through S moved with it (Source::world).
// The presenter submits that pair in place of the game-rate DIBR pair (which stays the fallback),
// with the eye poses it was rendered for, so the compositor's own reprojection only corrects its
// last prediction error. The game's UI on its own layers and the VR overlay are untouched; our
// in-frame items are part of the scene.
//
// Threading (the OpenXR frame thread, like VrOverlayRenderer): BeginFrame() before Submit takes
// the queue lock waits (1 ms at most) for the acquired swapchain image; Render() with the queue
// lock held records and submits the frame's GPU work (waiting on the capture's draw-stream tick,
// which is done in practice: the presenter shows a pair only once its GPU work is finished),
// releases the image and acquires the next one. Never waits on the GPU: a slot whose last work is
// still running shows the previous image again (at most ReuseFrames XR frames, then the fallback).
// Any Vulkan or OpenXR error disables it for the session (one log line).
class VrLateWarp {
public:
    struct DeviceContext {
        vk::PhysicalDevice physical_device;
        vk::Device device;
        VmaAllocator allocator{};
        vk::Queue queue;           // the graphics queue (the OpenXR session's too, unless the
                                   // runtime has its own: QueueHooks order the two then)
        u32 queue_family{};
        std::mutex* queue_mutex{}; // guards `queue` (Scheduler::submit_mutex)
    };

    template <typename InstanceT>
    static DeviceContext FromInstance(const InstanceT& instance, std::mutex& queue_mutex) {
        return {instance.GetPhysicalDevice(),       instance.GetDevice(),
                instance.GetAllocator(),            instance.GetGraphicsQueue(),
                instance.GetGraphicsQueueFamilyIndex(), &queue_mutex};
    }

    // One XR frame's warp: the capture on display and where V is relative to it.
    struct Source {
        VrStereoWarp::LateCapture capture{};
        std::array<vk::Rect2D, 2> crops{}; // each eye's crop of the render (V's pixels)
        // vr.ini eye_upscale / eye_sharpen (VrEyeUpscale): each eye's size in the swapchain (zero
        // or the crop's: as warped) and the RCAS strength (0: none).
        std::array<vk::Extent2D, 2> out{};
        float sharpen{0.0f};
        VR::Color::Settings color{};
        // vr.ini late_warp_edge_blend (VrLate::EdgeBlendScale): at a depth edge whose two sides
        // move apart by less than this many px between the capture and the view, the colour is
        // blended instead of the nearest texel (0: nearest everywhere).
        float edge_blend_px{0.0f};
        // vr.ini eye_taa (vr_eye_taa.comp): how much of the previous refreshes each eye keeps
        // (0: off). `taa_follow` (eye_taa_follow): the history also follows the game camera
        // (VrEyeTaa::Choose): `taa_views`, the displayed eyes in the game world (never submitted
        // to XR: eye_poses below stay tracking-space poses), when `taa_frame` is valid; else
        // eye_poses while the world stands still, as with taa_follow off. While the world moves:
        // `taa_gate_px` (eye_taa_gate_px) and `taa_edge_cap` (eye_taa_edge_cap).
        float taa{0.0f};
        bool taa_follow{false};
        float taa_gate_px{0.05f};
        float taa_edge_cap{0.5f};
        VrEyeTaa::Frame taa_frame{};
        std::array<VrEyeTaa::View, 2> taa_views{};
        std::array<XrFovf, 2> fovs{};      // ... and the FOV it is submitted with
        VrLate::Reprojection reprojection{};
        std::array<XrPosef, 2> eye_poses{}; // the virtual eyes the pair is rendered for
        vk::Semaphore gpu{};                // the capture's draw-stream timeline ...
        u64 gpu_tick{0};                    // ... and the tick that finished it
        std::array<u64, 4> stamp{};         // VR_STAMP rows (WriteStamp)
        // late_warp_motion: the world moved since the capture (the game camera walking or
        // turning): its texels are seen through `world` (V -> S moved with the world), the
        // hunter's body (`body`, the capture's camera frame) through `reprojection` as before.
        bool motion{false};
        VrLate::Reprojection world{};
        VrLate::BodyShapes body{};
        u64 capture_id{0}; // the capture and body the classification is for (0: none)
        // vr.ini vignette_in_warp (VrVignetteWarp): the vignettes DrawOverlay handed over, blended
        // into the eyes by the upscale's last pass (only when this warp enlarges or sharpens).
        VrVignetteWarp::Frame vignettes{};
        // vr.ini late_warp_fetch_once: pass 2 with the Catmull-Rom kernel's fetches shared (the
        // same image).
        bool fetch_once{false};
    };

    struct Stats {
        u32 rendered{};      // XR frames warped
        u32 reused{};        // ... that showed the last warp again (GPU busy, image not ready)
        u32 gpu_late{};      // ... of them: the slot's work from 2 XR frames ago still running
        u32 wait_timeouts{}; // ... of them: xrWaitSwapchainImage timed out (1 ms)
        double gpu_ms{};     // summed GPU time of finished warps
        double gpu_max_ms{};
        u32 gpu_samples{};
        u32 motion{};        // warps with the world moved (late_warp_motion)
        u32 classified{};    // captures classified into body and world texels
        double reproject_ms{}; // pass 1's share of gpu_ms (summed)
        double classify_ms{};  // the classification's (summed over classify_samples)
        u32 classify_samples{};
        u32 taa{};             // warps through the eyes' temporal AA (eye_taa) ...
        u32 taa_history{};     // ... of them with a history to blend (the rest started one)
        u32 taa_moving{};      // ... of those: moved with the game camera, the world moving
        u32 taa_head{};        // ... of those: moved by the head only (eye_taa_follow fallback)
        double taa_ms{};       // its pass's share of gpu_ms (summed over taa_samples)
        u32 taa_samples{};
        u32 upscaled{};        // warps enlarged or sharpened (eye_upscale / eye_sharpen)
        double upscale_ms{};   // their upscale pass's share of gpu_ms (summed over upscaled)
        u32 upscale_samples{};
        double warp_ms{};      // pass 2's share of gpu_ms (summed over gpu_samples)
        double copy_ms{};      // the copy into the swapchain image (summed over gpu_samples)
        u32 vignettes{};       // warps that blended vignettes into the eyes (vignette_in_warp)
        u32 fetch_once{};      // warps through the fetch-once pass 2 (late_warp_fetch_once)
        std::array<vk::Extent2D, 2> out{}; // the last warp's eye sizes in the swapchain
    };

    VrLateWarp();
    ~VrLateWarp();
    VrLateWarp(const VrLateWarp&) = delete;
    VrLateWarp& operator=(const VrLateWarp&) = delete;

    // Compiles the two pipelines and creates the frame resources (the swapchain and the images
    // come with the first capture, sized from it). `format`: the eye swapchains' (an 8-bit sRGB
    // format; the warp stores the bytes in that order). False (logged): off for the session.
    bool Init(const DeviceContext& device, XrSession session, s64 format);
    // Waits for its own GPU work, destroys everything including the swapchain. Before
    // xrDestroySession.
    void Shutdown();
    [[nodiscard]] bool IsActive() const {
        return initialized && !failed;
    }
    void Disable(std::string_view reason);

    // VR_STAMP: writes the frame stamp into the copied image (the presenter's WriteStamp).
    using StampWriter =
        std::function<void(vk::CommandBuffer, vk::Image, const XrRect2Di&, const std::array<u64, 4>&)>;
    void SetStampWriter(StampWriter writer) {
        stamp_writer = std::move(writer);
    }

    // Frame thread, before the queue lock: waits (1 ms at most) for the acquired image.
    void BeginFrame();
    // Queue lock held: warps `source` and fills both views (pose, FOV, swapchain rect). False: no
    // pair this frame (the caller shows the game-rate pair). `reuse_only`: show the last warp
    // again if allowed (no new capture warp this frame).
    bool Render(const Source& source, std::array<XrCompositionLayerProjectionView, 2>& views);
    // The last warp again (at most ReuseFrames frames after it), e.g. when no capture can be warped.
    bool Reuse(std::array<XrCompositionLayerProjectionView, 2>& views);

    Stats TakeStats();
    // VR latency (VR_LATENCY_GPU=1, the presenter's warp-done watcher): every warp's submission
    // also signals `semaphore` (a timeline semaphore; null: none) with the next value;
    // LastDoneValue() is the value the last submitted warp signals.
    void SetDoneSemaphore(vk::Semaphore semaphore) {
        done_semaphore = semaphore;
    }
    [[nodiscard]] u64 LastDoneValue() const {
        return done_value;
    }
    // The runtime's own queue (the presenter's VrXrQueue; unset with the shared queue: nothing
    // changes): the warp stays on the graphics queue, and these order it with the runtime's,
    // inside the warp's own batch (no empty batches). All on the frame thread, inside Render (both
    // queues' locks held).
    struct QueueSync {
        vk::Semaphore wait{}; // a timeline the warp's batch also waits for (all commands)
        u64 wait_value{0};
        vk::Semaphore signal{}; // a timeline the warp's batch also signals
        u64 signal_value{0};
    };
    struct QueueHooks {
        std::function<void()> after_acquire; // right after xrAcquireSwapchainImage
        // Before the warp's submit: its extra wait (the graphics queue after the runtime's work on
        // the acquired image) and signal (for the XR queue's wait before the release).
        std::function<QueueSync()> before_submit;
        // After a successful submit, before the release: the XR queue waits for the signal.
        std::function<void(const QueueSync&)> after_submit;
    };
    void SetQueueHooks(QueueHooks hooks) {
        queue_hooks = std::move(hooks);
    }
    [[nodiscard]] std::string Describe() const;
    [[nodiscard]] bool TaaAccumulating() const { return taa_used_history; }
    // The last recorded warp could have carried vignettes (it enlarged or sharpened, and the
    // vignette shaders exist): vignette_in_warp routes the next frame's here.
    [[nodiscard]] bool VignettesPossible() const {
        return vignettes_possible;
    }
    // ... and the world stood still (the camera's sub-pixel jitter, eye_taa_jitter: while the
    // world moves the body, held items and the near field get no history, so a jitter would only
    // make them shimmer).
    [[nodiscard]] bool TaaAccumulatingStill() const {
        return taa_used_history && !taa_choice.moving;
    }

private:
    struct Slot {
        vk::CommandBuffer cmd;
        vk::Fence fence;
        bool pending{false};
    };
    struct Image {
        vk::Image image;
        VmaAllocation allocation{};
        vk::ImageView view;
        vk::Extent2D extent{};
        vk::Format format{vk::Format::eUndefined};
    };
    struct Buffer {
        vk::Buffer buffer;
        VmaAllocation allocation{};
        void* mapped{};
    };

    bool Fail(std::string_view what);
    bool CreatePipelines();
    bool CreateFrameResources();
    bool EnsureImage(Image& image, vk::Extent2D extent, vk::Format format,
                     vk::ImageUsageFlags usage, std::string_view name);
    void DestroyImage(Image& image);
    bool EnsureSwapchain(vk::Extent2D extent);
    void DestroySwapchain();
    bool AcquireImage();
    void WaitIdle();
    void ReadTimestamps(u32 slot);
    bool Record(Slot& slot, u32 index, const Source& source, bool scaling,
                const std::array<vk::Extent2D, 2>& out);
    void FillViews(std::array<XrCompositionLayerProjectionView, 2>& views) const;

    static constexpr u32 ReuseFrames = 2;
    static constexpr u32 QueriesPerSlot = 7;

    DeviceContext dev{};
    XrSession session{XR_NULL_HANDLE};
    s64 xr_format{0};
    vk::Format vk_format{vk::Format::eUndefined};
    bool bgra{true};
    bool initialized{false};
    bool failed{false};

    // Swapchain: left eye at x = 0, right eye at x = eye_width[0].
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<vk::Image> swapchain_images;
    vk::Extent2D swapchain_extent{};
    u32 image_index{0};
    bool acquired{false};
    bool waited{false};

    Image q_map{};    // V -> S positions (RG32F)
    Image v_disp{};   // V's disparity + true inverse depth for temporal history (RG32F)
    Image v_tiles{};  // V's tiles (RGBA32F)
    bool q_map_cleared{false}; // V's images cleared once after (re)creation
    Image eyes{};     // both eyes side by side (RGBA8 UNORM storage, swapchain byte order)
    // eye_upscale / eye_sharpen: the warp writes each eye at the render's pixels here, and
    // VrEyeUpscale enlarges and sharpens them into `eyes`.
    std::array<Image, 2> eye_src{};
    VrEyeUpscale upscale;
    // eye_taa: per eye two RGBA16F histories (this warp writes taa_hist[e][taa_parity], reads the
    // other; .a the surface's 1/z in metres, VrEyeTaa::HistUnknown / HistNone), each eye pixel's
    // surface 1/z (both eyes side by side, R32F, from pass 2: VrEyeTaa::DecodeEyeW), and what the
    // last actually rendered warp's eyes were: their poses, game-world views and frame, intrinsics
    // (fx, fy, cx, cy in eye pixels) and sizes.
    std::array<std::array<Image, 2>, 2> taa_hist{};
    Image eye_w{};
    u32 taa_parity{0};
    bool taa_valid{false};         // taa_hist[e][taa_parity ^ 1] holds the last warp's eyes
    bool taa_hist_ready{false};    // the history images are in GENERAL layout
    std::array<std::array<float, 4>, 2> taa_prev_intr{};
    std::array<vk::Extent2D, 2> taa_prev_size{};
    std::array<XrPosef, 2> taa_prev_poses{};
    std::array<VrEyeTaa::View, 2> taa_prev_views{};
    VrEyeTaa::Frame taa_prev_frame{};
    VrEyeTaa::Choice taa_choice{}; // this warp's (Render, before Record)
    bool mask_body{false};         // this warp classifies the body (motion, or the TAA moving)
    std::array<bool, 2> slot_taa{}; // the slot's last work had the TAA pass
    bool taa_used_history{false};   // the last recorded warp blended a history (stats)
    float edge_blend_logged{-1.0f}; // late_warp_edge_blend last logged (Record)
    vk::DescriptorSetLayout taa_set_layout;
    vk::PipelineLayout taa_layout;
    vk::Pipeline taa_pipeline;
    std::array<bool, 2> slot_upscaled{}; // the slot's last work had the upscale pass
    // late_warp_motion: the capture's 1/z signed (body texels negative) and its layer tiles
    // (world min, max, body min, max), from vr_late_classify.comp once per capture.
    Image signed_w{};
    Image layer_tiles{};
    bool layers_ready{false}; // in GENERAL layout (written once)
    u64 classified_id{0};     // the Source::capture_id they hold
    Buffer uniforms{};
    vk::DeviceSize uniform_stride{};    // per slot: the reprojection's, then the classification's
    vk::DeviceSize classify_offset{};   // ... at this offset in the slot
    vk::DeviceSize vignette_offset{};   // ... and the vignettes' (VrVignetteWarp::Block)
    bool vignettes_possible{false};     // VignettesPossible()
    Buffer dummy{}; // the warp's depth buffer binding (never written: no FlagDepth)

    vk::DescriptorSetLayout reproject_set_layout;
    vk::PipelineLayout reproject_layout;
    vk::Pipeline reproject_pipeline;
    vk::DescriptorSetLayout warp_set_layout;
    vk::PipelineLayout warp_layout;
    vk::Pipeline warp_pipeline;
    vk::Pipeline warp_pipeline_once; // WARP_FETCH_ONCE (late_warp_fetch_once)
    vk::DescriptorSetLayout classify_set_layout;
    vk::PipelineLayout classify_layout;
    vk::Pipeline classify_pipeline;

    vk::CommandPool command_pool;
    std::array<Slot, 2> slots{};
    std::array<bool, 2> slot_classified{}; // the slot's last work classified a capture
    u32 slot_index{0};
    vk::QueryPool query_pool;
    float timestamp_period_ns{0.0f};
    u64 timestamp_mask{~u64{0}};

    // What the shown image was rendered for.
    bool has_image{false};
    u64 frame{0};
    u64 shown_frame{0};
    std::array<XrPosef, 2> shown_poses{};
    std::array<XrFovf, 2> shown_fovs{};
    std::array<XrRect2Di, 2> shown_rects{};

    StampWriter stamp_writer;
    QueueHooks queue_hooks; // SetQueueHooks
    vk::Semaphore done_semaphore{}; // SetDoneSemaphore
    u64 done_value{0};
    Stats stats{};
    u64 memory_bytes{0};
};

} // namespace Vulkan

#endif // ENABLE_OPENXR
