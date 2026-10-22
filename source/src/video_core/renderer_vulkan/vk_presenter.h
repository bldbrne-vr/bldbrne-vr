// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/vr/vital_color.h"
#include "core/vr/color_settings.h"
#include "core/vr/vitals_channel.h"
#include "core/vr/ui_conditions.h"
#include "core/vr/ui_layout.h"
#include "core/vr/vr_session.h"

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "imgui/imgui_texture.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/openxr_presenter.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vr_eye_upscale.h"
#include "video_core/renderer_vulkan/vr_native_fsr1_probe.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"
#include "video_core/renderer_vulkan/vr_ui_layer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    VR::FrameTag vr_tag{};
    // VR DIBR stereo (stereo_mode 2): both eyes were warped from this frame in PrepareFrame
    // (crop-sized, eTransferSrcOptimal, swapchain byte order; owned by the presenter).
    bool vr_stereo{false};
    std::array<std::unique_ptr<VideoCore::UniqueImage>,2> native_eyes;
    std::array<vk::UniqueImageView, 2> native_eye_views;
    std::array<vk::Image, 2> vr_eye_image{};
    std::array<vk::Extent2D, 2> vr_eye_extent{};
    std::array<vk::Rect2D, 2> vr_eye_rect{}; // each eye's crop of the render
    std::array<VR::Fov, 2> vr_eye_fov{};     // ... and the FOV it is submitted with
    float vr_eye_offset_m{};                 // eye poses: head -/+ right * this
    bool vr_eye_bgra{false}; // the eye images hold B8G8R8A8 bytes (the headset swapchains')
    // vr.ini desktop_mirror_view right / left: the window shows this published frame's eye
    // vr_desktop_eye_index (0 left, 1 right; Present); the window-only post-process pass did not
    // run, `image` holds nothing (eGeneral).
    bool vr_desktop_eye{false};
    u8 vr_desktop_eye_index{1};
    // The eyes were copied into the headset swapchains in the draw stream (RecordStereoCopy); the
    // present thread leaves the headset alone for this frame.
    bool vr_published{false};
    // VR UI layer (vk_presenter_vr.cpp): the frame's UI resolved onto its own layer; the eyes are
    // the scene without it.
    OpenXRPresenter::UiFrame vr_ui{};
    // ... and, when the eyes are not warped (AER, mono, DIBR refused), the mono eye image: the
    // scene without the UI, post-processed like `image` (eShaderReadOnlyOptimal).
    vk::Image vr_scene_image{};

    ImTextureID imgui_texture;
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;
class OpenXRPresenter;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address, const VR::FrameTag& vr_tag = {});

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

    // VR DIBR stereo (vk_presenter_vr.cpp): warps both eyes of the flipped frame from the
    // rasterizer's capture into the frame's eye images; anything missing leaves it mono.
    void RecordVrStereo(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size);
    void RecordNativeStereo(vk::CommandBuffer, Frame&, VideoCore::Image&, vk::Extent2D, vk::Format);
    // ... eye_upscale / eye_sharpen: `eyes` (eTransferSrcOptimal, left so) enlarged to `out` and
    // sharpened into vr_eye_scaled (eTransferSrcOptimal). False: not done (copy them as they are).
    bool UpscaleVrEyes(vk::CommandBuffer cmdbuf, VrStereoWarp::EyeImages& eyes,
                       const std::array<vk::Extent2D, 2>& out, float sharpen);
    // VR UI layer (vk_presenter_vr.cpp): resolves the frame's UI capture into its atlas and takes
    // the game's dim into the scene (before the eyes are made from it).
    void RecordVrUi(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size);
    // ... after RecordVrStereo: the mono eye without the UI when the eyes were not warped, and the
    // UI's copy into the headset when the eyes were (draw stream).
    void RecordVrUiEyes(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size);
    // vr.ini desktop_mirror_view right / left (Present): that eye of the frame, centre-cropped to the
    // window's shape, into the swapchain image (left in eTransferDstOptimal; black when it
    // cannot be shown). core/vr/desktop_mirror.h.
    void RecordMirrorEye(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Image target,
                         vk::Extent2D extent);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};
    vk::Extent2D game_extent{};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    std::unique_ptr<HostPasses::PostProcessingPass> native_feedback_pass;
    vk::Format native_feedback_format{};
    VR::VitalColor::State native_feedback_state;
    s64 native_feedback_ns{};
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    std::unique_ptr<OpenXRPresenter> openxr_presenter;
    std::unique_ptr<Rasterizer> rasterizer;
    // VR DIBR stereo (stereo_mode 2): the warp (recorded into draw_scheduler) and each
    // presentation frame's eye images (by Frame::id).
    std::unique_ptr<VrStereoWarp> vr_warp;
    std::vector<VrStereoWarp::EyeImages> vr_eye_images;
    // vr.ini eye_upscale / eye_sharpen for the eyes copied into the headset in the draw stream
    // (RecordStereoCopy): FSR 1 from the warped eyes into these (VrEyeUpscale, made at first use).
    std::unique_ptr<VrEyeUpscale> vr_upscale;
    std::unique_ptr<VrNativeFsr1Probe> native_fsr1_probe;
    bool vr_upscale_failed{false};
    VrStereoWarp::EyeImages vr_eye_scaled;
    // VR UI layer: K/W targets, scene copy, resolve (recorded into draw_scheduler).
    std::unique_ptr<VrUiLayer> vr_ui_layer;
    struct VrUiStats {
        u32 flips{};      // flips with VR on
        u32 ui_frames{};  // frames drawn with the UI layer (FrameTag::ui_layer 1)
        u32 armed{};      // ... whose UI was shadowed
        u32 layer{};      // ... resolved onto the layer
        u32 empty{};      // ... without any UI draw
        u32 incomplete{}; // ... with X evicted or written by compute in the UI phase
        u32 no_marker{};  // ... without the composite (menus without it, loading)
        u32 refused{};    // ... refused at the composite (today's path)
        u32 held{};       // ... of them menu / dialog frames held (the last eyes and canvas)
        u32 page_held{};  // full menu frames without the menu held at the VR page's open
        const char* refusal{};
        u32 mono_eyes{}; // ... whose eyes were made from the scene copy (not warped)
        u32 checked{};   // ui_layer=2 frames compared
        u64 shadowed{};  // shadowed draws, offtarget, backdrop, non-affine, destination alpha
        u64 offtarget{};
        u64 backdrop{};
        u64 nonaffine{};
        u64 dst_alpha{};
        u64 ui_draws{};
        std::array<u32, 4> classes{}; // UI frames per VR::UiClass
    } vr_ui_stats;
    u32 vr_ui_held_run{}; // refused menu / dialog frames held in a row (RecordVrUi)
    // Gameplay UI reaching the screen's edges (the death screen, a loading page): shown whole on
    // the rest quad (VR::Ui::StepScreenWide; GPU thread).
    VR::Ui::ScreenWideState vr_ui_screen_wide{};
    bool vr_ui_area_over_wide{false}; ///< an area name kept out of the screen-wide picture (log)
    VR::Ui::AreaNameState vr_ui_area{}; ///< an area name up (GPU thread; UiFrame::area_name)
    s64 vr_ui_area_ns{0};               ///< its last step (steady ns)
    bool vr_ui_was_immersive{false};  ///< VR::IsImmersive() last frame (back in VR: wide off)
    s64 vr_ui_wide_hold_ns{0};        ///< the screen-wide picture held off until then
    bool vr_ui_lamp_hidden{false};    ///< no UI around a lantern warp (RecordVrUi)
    bool vr_ui_lamp_scene{false};     ///< ... the arrival's lamp scene has started
    // The status build-up meter's rows held at their largest (ui_conditions; GPU thread) and
    // whether the last frame cut it out (the log).
    VR::Ui::Conditions::Hold vr_ui_conditions_hold{};
    bool vr_ui_conditions_on{false};
    struct VrDibrStats {
        u32 flips{};          // flips with VR on
        u32 dibr_flips{};     // immersive frames rendered in stereo_mode 2
        u32 stereo{};         // ... warped into both eyes
        u32 no_capture{};     // no composite capture (menus, loading, a refused capture)
        u32 record_skew{};    // the draws used another camera update than the flip's tag
        u32 skew_newer{};     // ... a newer one than the flip's
        u32 skew_flip_sent{}; // ... and the frame went out with the flip's (VR_TAG_SOURCE=flip)
        // Head rotation between the flip's and the draws' camera updates (deg): what the frame
        // would have been turned by in the compositor with the flip's pose.
        double skew_deg_sum{};
        float skew_deg_max{};
        std::array<float, 300> skew_deg{};
        u32 camera_mismatch{}; // ... of another projection or eye distance
        u32 hdr{};            // HDR output
        u32 swapchain{};      // eye swapchains missing, not 8-bit sRGB, or a crop does not fit
        u32 eye_limit{};      // an eye larger than the runtime's largest image rect / swapchain
        u32 size{};           // the capture is not the flipped frame's size
        u32 no_hud{};         // the composite target went away before the flip
        u32 warp_failed{};
    } vr_dibr_stats;
    bool vr_dibr_format_warned{false}; // RecordVrStereo: the no-8-bit-sRGB-swapchain error logged
    // vr.ini desktop_mirror / desktop_mirror_view (present thread): the frames published to the
    // headset so far (the every-Nth count) and the eye blit's intermediate (the window's size;
    // RecordMirrorEye).
    u64 mirror_published{0};
    u64 mirror_skipped{0};
    u64 mirror_eye_shown{0};
    VideoCore::UniqueImage mirror_image;
    vk::Extent2D mirror_extent{};
    vk::Format mirror_format{vk::Format::eUndefined};
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::optional<ImGui::RefCountedTexture> splash_img;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
