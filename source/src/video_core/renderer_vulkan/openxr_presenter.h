// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"

namespace Vulkan {

class Instance;
class MasterSemaphore;

// OpenXR presentation for Bloodborne VR.
//
// A dedicated thread runs the OpenXR frame loop at the headset refresh rate. It
// publishes the predicted head pose (which drives the game camera), polls the
// controllers into the emulated pad, and submits the most recent image of each eye
// together with the pose that image was rendered for, so the runtime reprojects each
// eye independently. Gameplay frames are shown as a projection layer; menus,
// cutscenes and loading screens are shown on a large virtual screen. With DIBR stereo
// (stereo_mode 2) both eyes are synthesised from one game frame in PrepareFrame and submitted
// together (CopyStereoFrame).
class OpenXRPresenter {
public:
    static void SetRequested(bool requested);
    [[nodiscard]] static bool IsRequested();
    [[nodiscard]] static bool IsRuntimeAvailable();
    // A headset is connected and reports its eye sizes (asleep is fine): a throwaway instance,
    // safe to call before IsRuntimeAvailable and from any thread until then.
    [[nodiscard]] static bool IsHeadsetConnected();

    // These methods initialize the OpenXR instance on demand and expose the
    // Vulkan requirements before shadPS4 creates its Vulkan instance/device.
    [[nodiscard]] static const std::vector<std::string>& RequiredVulkanInstanceExtensions();
    [[nodiscard]] static const std::vector<std::string>& RequiredVulkanDeviceExtensions();
    [[nodiscard]] static vk::PhysicalDevice RequiredVulkanPhysicalDevice(vk::Instance instance);
    // The runtime's XrInstanceProperties::runtimeName ("SteamVR/OpenXR", "Oculus"; empty before
    // the instance or without one): the device's queues depend on it (vr_xr_queue_policy.h).
    [[nodiscard]] static std::string RuntimeName();
    static void Disable(std::string_view reason);

    OpenXRPresenter(const Instance& instance, vk::Format frame_format);
    ~OpenXRPresenter();

    OpenXRPresenter(const OpenXRPresenter&) = delete;
    OpenXRPresenter& operator=(const OpenXRPresenter&) = delete;

    [[nodiscard]] bool IsReady() const;

    // Present thread: records a copy of a finished guest frame into the headset
    // swapchain of the eye it was rendered for. source_image must be in
    // eShaderReadOnlyOptimal on entry and is restored to that layout.
    void CopyFrame(vk::CommandBuffer command_buffer, vk::Image source_image,
                   vk::Extent2D source_extent, const VR::FrameTag& tag);

    // Present thread: called after the command buffer passed to CopyFrame or CopyStereoFrame
    // was submitted (as `gpu_tick` of `gpu`). Releases the swapchain image(s) and publishes them
    // to the frame loop.
    void ReleaseFrame(MasterSemaphore* gpu = nullptr, u64 gpu_tick = 0);

    // ---- DIBR stereo (stereo_mode 2) ----
    // One eye's view of a symmetric render: the FOV it is submitted with (the located one on a
    // fixed-FOV runtime) and the render pixels that cover it.
    struct EyeCrop {
        VR::Fov fov{};
        vk::Rect2D rect{};
    };
    // Both eyes' crops of a `render`-sized frame rendered with camera `record`, exactly as
    // CopyFrame submits them (any thread).
    [[nodiscard]] std::array<EyeCrop, 2> EyeCropsFor(const VR::CameraRecord& record,
                                                     vk::Extent2D render) const;
    // The centre image's eye swapchains' format and size (eUndefined / 0x0 until the first frame
    // created them; any thread). The DIBR pairs use the same format.
    [[nodiscard]] vk::Format EyeSwapchainFormat() const;
    [[nodiscard]] vk::Extent2D EyeSwapchainExtent() const;

    // Both eye images of one game frame (warped in PrepareFrame, renderer_vulkan/vr_stereo_warp).
    struct StereoEyes {
        std::array<vk::Image, 2> image{};    // crop-sized (or EyeOutputExtents'),
                                             // eTransferSrcOptimal, swapchain bytes
        std::array<vk::Extent2D, 2> extent{};
        std::array<EyeCrop, 2> crop{};       // where each image belongs in the render
        float eye_offset_m{};                // eye poses: head -/+ right * this (the warp's baseline)
        bool native{false}; // the game drew both eyes (vr_native_stereo): true depth, no DIBR warp
        // Their depth (RecordStereoCopy only: VrStereoWarp::DepthBuffer, written in the same draw
        // stream): one float per eye pixel in crop rows from depth_offset, 0..1 between
        // depth_near_m and depth_far_m. No buffer: no depth.
        vk::Buffer depth_buffer{};
        std::array<vk::DeviceSize, 2> depth_offset{};
        float depth_near_m{};
        float depth_far_m{};
        // The late warp's source (VrStereoWarp::GetLateCapture; invalid without vr.ini
        // late_warp): while the pair is shown, the frame thread warps it again every XR frame.
        VrStereoWarp::LateCapture late{};
    };
    // True while DIBR eyes are submitted with their depth (vr.ini xr_depth, the runtime has
    // XR_KHR_composition_layer_depth and a float depth swapchain format; any thread).
    [[nodiscard]] bool DepthWanted() const;
    // vr.ini eye_upscale: each eye's size in the headset swapchains for eye crops of `crops`
    // render pixels (the crops themselves when off; any thread). The DIBR copy enlarges the eyes
    // to it with VrEyeUpscale; StereoEyes::extent is then this size.
    [[nodiscard]] std::array<vk::Extent2D, 2> EyeOutputExtents(
        const std::array<vk::Extent2D, 2>& crops) const;
    // DIBR eyes of `eyes` pixels (their crops of the render) fit the runtime's largest eye image
    // rect and swapchain (vr_xr_size.h CheckDibrEyes; any thread). Each eye has its own DIBR
    // swapchain, so the render's own size and the centre image's swapchains (EyeSwapchainExtent,
    // scaled when the render is larger than the largest swapchain) play no part. False (or not
    // ready): the frame goes mono, with an error line.
    [[nodiscard]] bool DibrEyesFit(const std::array<vk::Extent2D, 2>& eyes) const;
    // Present thread: copies both eyes into the headset swapchains; ReleaseFrame publishes them
    // together, so both eyes always show the same game frame. Falls back to CopyFrame with
    // source_image (the centre image to both eyes) when the eyes do not fit the swapchains.
    // Returns true when both eyes were copied (false: the fallback, or nothing).
    bool CopyStereoFrame(vk::CommandBuffer command_buffer, const StereoEyes& eyes,
                         vk::Image source_image, vk::Extent2D source_extent,
                         const VR::FrameTag& tag);

    // ---- DIBR eyes handed over in the draw stream (the default) ----
    // GPU thread (PrepareFrame, right after the warp): copies both eyes into the pair of DIBR
    // swapchains that is not on display, recorded into `command_buffer` (the draw stream), so the
    // copy runs right after the frame's own work instead of behind the next frame's on the present
    // thread. False: nothing recorded (no eye swapchains yet, headset not visible, a swapchain
    // image not ready, VR_EYE_HANDOFF=present): the present thread copies the eyes as before.
    bool RecordStereoCopy(vk::CommandBuffer command_buffer, const StereoEyes& eyes,
                          const VR::FrameTag& tag);
    // ---- The late warp (vr.ini late_warp, renderer_vulkan/vr_late_warp.h) ----
    // True when the frame thread warps the shown DIBR pair again every XR frame (the setting is on
    // and its pipelines are ready); the captures must then rotate through VrStereoWarp's slots
    // (EnableLateRing with LateBusyMask: the slots the presenter still shows or has queued).
    [[nodiscard]] bool LateWarpEnabled() const;
    [[nodiscard]] const std::atomic<u32>* LateBusyMask() const;
    // The capture slots the late warp needs: the shown pair's, the waiting ones' (vr.ini
    // stereo_adopt_finished) and the next capture's (EnableLateRing).
    [[nodiscard]] u32 LateSlotsWanted() const;
    // Before the captures go away (Presenter's destructor): no more late warps, the GPU idle.
    void ShutdownLateWarp();

    // GPU thread, after the command buffer RecordStereoCopy recorded into was submitted (as
    // `gpu_tick` of `gpu`): releases both images and hands the pair to the frame thread, which
    // shows it once that GPU work is done (never an unfinished image) and the shown pair has been up
    // for a whole game frame (an even cadence).
    void PublishStereo(MasterSemaphore* gpu, u64 gpu_tick);

    // ---- The game's UI on its own layers (renderer_vulkan/vr_ui_layer.h) ----
    // One frame's UI, resolved in PrepareFrame: the atlas holds the whole 16:9 band of the render
    // (top half) and the band without the HUD band's regions (bottom half), premultiplied, in the
    // swapchain's byte order. The presenter shows it as transparent layers over the eyes.
    struct UiFrame {
        bool valid{false};     // the frame's UI is on the layer (its eye images have none)
        bool empty{false};     // ... and the frame drew no UI
        vk::Image image{};     // the atlas, eTransferSrcOptimal (written in the draw stream)
        vk::ImageView view{};  // ... its view (FAST_GPU_UI_COPY: sampled by the rest's warp)
        vk::Extent2D extent{}; // atlas size: band width x twice the band height (+ the strip)
        vk::Extent2D band{};   // the band's size (render pixels)
        vk::Extent2D render{}; // the render's size
        // The bounding box of the UI in the middle of the screen (band px, end exclusive), a few
        // frames old: where a dialog is.
        bool box_valid{false};
        std::array<s32, 4> box{};
        // Which parts held any UI, as old as the box: bits 0..3 the HUD band's segments, bit 4 the
        // rest.
        u32 regions{0x1F};
        // The frame the box was measured in (FrameTag::seq) and its game menu class: a dialog's
        // crop only takes boxes of the dialog's own frames.
        u64 box_seq{0};
        u8 box_class{0};
        // Band pixels holding UI and opaque UI in that frame (VrUiLayer::ContentBox): a
        // screen-wide box of an opaque page (Status) vs a dim over the menu.
        u32 box_content{0};
        u32 box_opaque{0};
        std::array<s32, 4> opaque_box{}; // the opaque UI's own box (band px; all 0: none)
        // Each HUD band region's own content box (band px x0, y0, x1, y1; valid where `regions`
        // has its bit), as old as the box: what of a HUD piece a hand can be in front of.
        std::array<std::array<s32, 4>, 4> cut_box{};
        // The right end of the lower group's HUD elements, as old as the box (band px, end
        // exclusive; 0: none): an area name in the lower-right corner (VR::Ui::StepAreaName).
        s32 hud_right{};
        // A gameplay frame whose UI reaches the screen's edges (the death screen, a loading
        // page; VR::Ui::StepScreenWide): shown whole on the rest quad, no HUD band pieces.
        bool screen_wide{false};
        bool dialogue{false}; // crop/routing belongs to this atlas, not the latest game state
        // An area name is up in this atlas (VR::Ui::StepAreaName on the GPU thread): its corner on
        // the lower piece, keyed, down to the stage's bottom (VR::Ui::CentreAreaName).
        bool area_name{false};
        // A full menu's frame whose atlas holds the HUD the game drew under the menu in the
        // bottom half's status cuts (ui_menu_hud; VrUiLayer::ResolveParams::menu_hud): HP,
        // stamina and the items shown under the menu canvas.
        bool menu_hud{false};
        // The status build-up meter's rows (ui_conditions=view; VR::Ui::Conditions) were cut
        // into the atlas's strip: their band rect (x0, y0, x1, y1) and the strip's first row.
        bool conditions{false};
        std::array<s32, 4> conditions_rect{};
        u32 strip_top{0};
    };
    // GPU thread (PrepareFrame), right after RecordStereoCopy recorded the frame's eyes: copies its
    // UI into a UI swapchain in the same draw stream; PublishStereo publishes both together.
    void RecordUiCopy(vk::CommandBuffer command_buffer, const UiFrame& ui, const VR::FrameTag& tag);
    // Present thread, right after CopyFrame / CopyStereoFrame: the same for a frame the present
    // thread copies; ReleaseFrame publishes both together.
    void CopyUiFrame(vk::CommandBuffer command_buffer, const UiFrame& ui, const VR::FrameTag& tag);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
