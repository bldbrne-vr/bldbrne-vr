// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR additions to the presenter: DIBR stereo (stereo_mode 2) and the UI layer at the flip.
//
// The rasterizer captured this frame's scene depth (as per-eye disparity) and its pre-HUD colour
// at the game's composite (vk_rasterizer_vr.cpp). Here, in the same draw stream right after the
// frame's last draw, the final image (scene + HUD) is copied too, and both eyes are warped from
// the one frame into the presentation frame's eye images, each exactly the crop CopyFrame would
// submit for it, and copied on into the free pair of DIBR swapchains in the same draw stream; the
// frame thread shows the pair once the GPU has finished it (OpenXRPresenter::RecordStereoCopy /
// PublishStereo; the present thread's CopyStereoFrame is the fallback). Both eyes always show the
// same game frame, at the game's frame rate. Every failed check leaves the frame on the mono path
// (CopyFrame, the centre image to both eyes) and is counted in the `VR dibr gpu:` line.
//
// UI layer (renderer_vulkan/vr_ui_layer.h): in a UI-layer frame the rasterizer drew the game's UI
// into K and W too, at native size, and copied the scene at the composite. Here the UI is
// resolved into the frame's atlas (the presenter shows it on its own compositor layers), the
// game's full-screen dim is taken into the scene copy, and the eyes are made from that copy: the
// DIBR warp without a HUD, or (AER, mono, a frame DIBR could not warp) the copy post-processed
// like the frame itself. The frame's own image keeps scene + UI (the desktop window).

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

#include "common/logging/log.h"
#include "core/vr/conditions_channel.h"
#include "core/vr/live_tune.h"
#include "core/vr/ui_conditions.h"
#include "core/vr/ui_layout.h"
#include "core/vr/vitals_channel.h"
#include "core/vr/vr_page_runtime.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/openxr_presenter.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"
#include "video_core/renderer_vulkan/vr_ui_layer.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {

namespace {

bool IsFloatFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eE5B9G9R9UfloatPack32:
    case vk::Format::eR16G16B16Sfloat:
        return true;
    default:
        return false;
    }
}

// The capture's camera matches what camera update `record` renders with (the projection, eye
// distance and scale the disparity was computed from).
bool SameCamera(const VrStereo::CameraParams& captured, const VR::CameraRecord& record,
                vk::Extent2D size) {
    if (!record.has_projection) {
        return false;
    }
    const auto near = [](float a, float b, float tolerance = 1e-5f) {
        return std::abs(a - b) <= tolerance * std::max(std::abs(a), std::abs(b));
    };
    const float aspect = record.proj_aspect > 0.0f
                             ? record.proj_aspect
                             : static_cast<float>(size.width) / static_cast<float>(size.height);
    const float half_ipd =
        0.5f * record.ipd * record.world_scale * VR::GetSettings().dibr_baseline_scale;
    // The near plane moves between camera updates while fp_near_down_m ramps it with the view
    // pitch. With the far plane thousands of near planes away, depth decoded with a near plane off
    // by a fraction puts every disparity off by about that fraction (1/z ~ n_used / (n * z)); up
    // to 10% (a fast nod through the ramp, for one frame) is much better than the frame going
    // mono, so it counts as the same camera.
    return near(captured.near_z, record.near_z, 0.1f) && near(captured.far_z, record.far_z) &&
           near(captured.vfov, record.proj_vfov) && near(captured.aspect, aspect) &&
           near(captured.half_ipd, half_ipd) && near(captured.units_per_metre, record.world_scale);
}

} // Anonymous namespace

void Presenter::RecordVrStereo(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size) {
    const VR::FrameTag& tag = frame.vr_tag;
    if (!tag.valid) {
        return;
    }
    auto& stats = vr_dibr_stats;
    struct Report {
        VrDibrStats& stats;
        VrStereoWarp& warp;
        ~Report() {
            if (++stats.flips < 300) {
                return;
            }
            const auto times = warp.TakeGpuTimes();
            if (stats.dibr_flips != 0) {
                const u32 measured = std::min<u32>(stats.record_skew, stats.skew_deg.size());
                std::sort(stats.skew_deg.begin(), stats.skew_deg.begin() + measured);
                const float p95 =
                    measured ? stats.skew_deg[std::min(measured - 1, (measured * 95) / 100)]
                             : 0.0f;
                LOG_INFO(Render_Vulkan,
                         "VR dibr gpu: {} flips, {} DIBR frames, {} warped to both eyes ({} drawn "
                         "with an older camera update than the flip's, {} newer; sent with the "
                         "flip's {} (0 = every frame with its own pose); head turned between the "
                         "two avg {:.2f} / p95 {:.2f} / max {:.2f} deg); mono because: no "
                         "capture {}, camera mismatch {}, hdr {}, swapchain {}, eye too large {}, "
                         "size {}, no hud {}, warp {}{}",
                         stats.flips, stats.dibr_flips, stats.stereo,
                         stats.record_skew - stats.skew_newer, stats.skew_newer,
                         stats.skew_flip_sent,
                         measured ? stats.skew_deg_sum / measured : 0.0, p95, stats.skew_deg_max,
                         stats.no_capture, stats.camera_mismatch, stats.hdr, stats.swapchain,
                         stats.eye_limit, stats.size, stats.no_hud, stats.warp_failed,
                         times.frames == 0
                             ? std::string{}
                             : fmt::format("; GPU prep {:.3f} ms (max {:.3f}), HUD copy + warp "
                                           "{:.3f} ms (max {:.3f}) over {} frames",
                                           times.prep_ms, times.prep_max_ms, times.warp_ms,
                                           times.warp_max_ms, times.frames));
                // Most gameplay frames of the window refused for a reason other than a menu or
                // loading screen (no capture): loud, not only the counts above.
                const u32 refused = stats.camera_mismatch + stats.hdr + stats.swapchain +
                                    stats.eye_limit + stats.size + stats.no_hud +
                                    stats.warp_failed;
                if (stats.dibr_flips >= 60 && 2 * stats.stereo < stats.dibr_flips &&
                    2 * refused > stats.dibr_flips) {
                    LOG_ERROR(Render_Vulkan,
                              "VR dibr: only {} of {} DIBR frames reached both eyes (refused: "
                              "swapchain {}, eye too large {}, size {}, camera mismatch {}, hdr "
                              "{}, no hud {}, warp {}): the headset shows MONO frames (no stereo "
                              "depth, no late warp)",
                              stats.stereo, stats.dibr_flips, stats.swapchain, stats.eye_limit,
                              stats.size, stats.camera_mismatch, stats.hdr, stats.no_hud,
                              stats.warp_failed);
                }
            }
            stats = {};
        }
    } report{stats, *vr_warp};
    if (!openxr_presenter || !tag.immersive || !tag.dibr) {
        return; // not a DIBR frame
    }
    frame.vr_tag.aux_record = rasterizer->VrCompositeRecord();
    ++stats.dibr_flips;
    if (pp_settings.hdr != 0) {
        ++stats.hdr;
        return;
    }
    if (!vr_warp->HasCapture()) {
        ++stats.no_capture; // no composite this frame (menus, loading), or a refused capture
        return;
    }
    const auto& info = vr_warp->GetCaptureInfo();
    if (info.frame_tag != tag.flip_record) {
        // The capture took the camera update the frame's draws used (the one current at its
        // first draw, Rasterizer::VrCaptureDepth), which the frame is tagged with
        // (VR::OnGpuFrameBoundary). The flip's update is the newest when the game submitted the
        // flip, by then often already the next frame's (Game:Main places the next camera while
        // GXRenderThread still builds this frame): with it the frame would be turned by the head
        // motion of one game frame on these frames only (jitter). Counted for the log line.
        VR::CameraRecord d{};
        VR::CameraRecord flipped{};
        if (stats.record_skew < stats.skew_deg.size() && VR::GetRecord(info.frame_tag, d) &&
            VR::GetRecord(tag.flip_record, flipped)) {
            const VR::Quat a = VR::QuatFromYawPitchRoll(d.head_yaw, d.head_pitch, d.head_roll);
            const VR::Quat b =
                VR::QuatFromYawPitchRoll(flipped.head_yaw, flipped.head_pitch, flipped.head_roll);
            const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            const float deg = 2.0f * std::acos(std::min(dot, 1.0f)) * 57.2957795f;
            stats.skew_deg[stats.record_skew] = deg;
            stats.skew_deg_sum += deg;
            stats.skew_deg_max = std::max(stats.skew_deg_max, deg);
        }
        ++stats.record_skew;
        stats.skew_newer += info.frame_tag > tag.flip_record ? 1 : 0;
    }
    if (info.frame_tag != tag.record.id) {
        // The capture's update is not the tag's: VR_TAG_SOURCE=flip (the tag is the flip's, the
        // old behaviour, kept for before/after measurements), or a camera update landed between
        // the two reads at the frame's first draw. The capture's wins (its disparity and the
        // submitted pose belong together), unless the test switch is on.
        VR::CameraRecord drawn{};
        if (!VR::GetRecord(info.frame_tag, drawn) || !SameCamera(info.camera, drawn, info.size) ||
            !drawn.driven || drawn.menu) {
            ++stats.camera_mismatch;
            return;
        }
        if (VR::FlipTagTest()) {
            ++stats.skew_flip_sent;
        } else {
            frame.vr_tag.record = drawn; // `tag` refers to it: the crops and poses below use it
        }
    }
    if (info.size != game_size || frame.width != game_size.width ||
        frame.height != game_size.height) {
        ++stats.size;
        return;
    }
    // The DIBR eyes go to their own swapchains (OpenXRPresenter::RecordStereoCopy, one eye each,
    // sized from its crop): only the format is shared with the centre image's swapchains, never
    // their size. (Until 2026-10-07 those had to be the render's size; on Meta Link, whose
    // largest eye image rect is 2848x3136, a 3200-wide render's were scaled to 2848, so every
    // frame went mono: no stereo depth, no late warp, no render pitch bias. vr_xr_size.h.)
    const vk::Format swapchain_format = openxr_presenter->EyeSwapchainFormat();
    if (swapchain_format != vk::Format::eB8G8R8A8Srgb &&
        swapchain_format != vk::Format::eR8G8B8A8Srgb) {
        ++stats.swapchain; // not created yet (first frames), or not an 8-bit sRGB copy
        if (swapchain_format != vk::Format::eUndefined && !vr_dibr_format_warned) {
            vr_dibr_format_warned = true;
            LOG_ERROR(Render_Vulkan,
                      "VR dibr: the headset swapchains are {} (no 8-bit sRGB format): DIBR "
                      "frames go MONO (the centre image to both eyes)",
                      vk::to_string(swapchain_format));
        }
        return;
    }
    const auto crops = openxr_presenter->EyeCropsFor(tag.record, game_size);
    for (const auto& crop : crops) {
        const vk::Rect2D& r = crop.rect;
        if (r.extent.width == 0 || r.extent.height == 0 || r.offset.x < 0 || r.offset.y < 0 ||
            r.offset.x + r.extent.width > game_size.width ||
            r.offset.y + r.extent.height > game_size.height) {
            ++stats.swapchain;
            return;
        }
    }
    if (!openxr_presenter->DibrEyesFit({crops[0].rect.extent, crops[1].rect.extent})) {
        ++stats.eye_limit; // an eye larger than the runtime allows (logged by DibrEyesFit)
        return;
    }
    // A UI-layer frame: the UI is on its own layer, the eyes show the scene alone.
    const bool ui_layer = frame.vr_ui.valid;
    vk::Format x_format = info.color_format;
    if (!ui_layer) {
        VideoCore::Image* composite = rasterizer->VrCompositeTarget();
        if (composite == nullptr) {
            ++stats.no_hud;
            return;
        }
        // The composite target now holds scene + HUD: the HUD is where it differs from the scene
        // copy.
        x_format = composite->backing->image.image_ci.format;
        composite->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                           {}, cmdbuf);
        vr_warp->CaptureHud(cmdbuf, composite->GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            x_format, info.size,
                            static_cast<vk::SampleCountFlagBits>(composite->backing->num_samples),
                            rasterizer->VrCompositeComponents());
    }

    const auto& s = VR::GetSettings();
    VrStereoWarp::Settings settings{};
    settings.hud_depth_m = s.hud_depth_m;
    settings.gamma = pp_settings.gamma;
    settings.color = VR::Color::Get();
    settings.hud_mode = ui_layer ? VrStereoWarp::HudMode::None : VrStereoWarp::HudMode::Diff;
    settings.source_encoding = IsFloatFormat(x_format) ? VrStereoWarp::SourceEncoding::Linear
                                                       : VrStereoWarp::SourceEncoding::Srgb;
    settings.catmull_rom = s.dibr_quality >= 1;
    settings.debug_holes = s.dibr_debug_holes >= 1;
    settings.debug_hud = s.dibr_debug_holes >= 2;
    // The eyes' depth for the compositor's positional timewarp (submitted by RecordStereoCopy).
    settings.depth = !tag.hold && openxr_presenter->DepthWanted();
    settings.depth_debug_m = s.xr_depth_debug_m;
    auto& eyes = vr_eye_images[frame.id];
    const bool bgra = swapchain_format == vk::Format::eB8G8R8A8Srgb;
    if (!vr_warp->WarpToEyeImages(cmdbuf, eyes, {crops[0].rect, crops[1].rect}, settings,
                                  vk::ImageLayout::eTransferSrcOptimal, bgra, {})) {
        ++stats.warp_failed;
        return;
    }
    for (u32 i = 0; i < 2; ++i) {
        frame.vr_eye_image[i] = eyes.image[i];
        frame.vr_eye_extent[i] = eyes.extent[i];
        frame.vr_eye_rect[i] = crops[i].rect;
        frame.vr_eye_fov[i] = crops[i].fov;
    }
    frame.vr_eye_offset_m = VrStereo::EyeOffsetMetres(info.camera);
    frame.vr_eye_bgra = bgra;
    frame.vr_stereo = true;
    ++stats.stereo;
    if (!tag.hold) {
        // Into the headset swapchains right here, after the frame's own work; PrepareFrame
        // publishes them after the flush (else the present thread copies them at its vblank).
        OpenXRPresenter::StereoEyes stereo{};
        for (u32 i = 0; i < 2; ++i) {
            stereo.image[i] = frame.vr_eye_image[i];
            stereo.extent[i] = frame.vr_eye_extent[i];
            stereo.crop[i] = {frame.vr_eye_fov[i], frame.vr_eye_rect[i]};
        }
        // vr.ini eye_upscale / eye_sharpen: the eyes enlarged (FSR 1) before the headset; not
        // with their depth (it stays at the render's pixels), and not while the late warp shows
        // the eyes (it enlarges its own every refresh; this pair is then only its fallback, so
        // the game frame's GPU work stays as short as before).
        if (!settings.depth && !openxr_presenter->LateWarpEnabled()) {
            const std::array<vk::Extent2D, 2> in{eyes.extent[0], eyes.extent[1]};
            const auto out = openxr_presenter->EyeOutputExtents(in);
            const float sharpen = s.eye_sharpen;
            if (out != in || sharpen > 0.0f) {
                GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0020ull,
                                        GpuPassProfiler::Group::VrHost,
                                        "VR eye upscale (FSR 1) and sharpen");
            }
            if ((out != in || sharpen > 0.0f) && UpscaleVrEyes(cmdbuf, eyes, out, sharpen)) {
                for (u32 i = 0; i < 2; ++i) {
                    stereo.image[i] = vr_eye_scaled.image[i];
                    stereo.extent[i] = out[i];
                }
            }
        }
        stereo.eye_offset_m = frame.vr_eye_offset_m;
        // The late warp's source: this capture, with the settings its eyes were warped with.
        stereo.late = vr_warp->GetLateCapture(settings);
        if (settings.depth) {
            stereo.depth_buffer = vr_warp->DepthBuffer();
            stereo.depth_offset = {vr_warp->DepthOffset(0), vr_warp->DepthOffset(1)};
            stereo.depth_near_m = VrStereoWarp::DepthNearM;
            stereo.depth_far_m = VrStereoWarp::DepthFarM;
        }
        GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0012ull,
                                GpuPassProfiler::Group::VrHost,
                                "VR eye copy into the XR swapchains");
        frame.vr_published = openxr_presenter->RecordStereoCopy(cmdbuf, stereo, frame.vr_tag);
    }
}

bool Presenter::UpscaleVrEyes(vk::CommandBuffer cmdbuf, VrStereoWarp::EyeImages& eyes,
                              const std::array<vk::Extent2D, 2>& out, float sharpen) {
    if (vr_upscale_failed || !eyes.image[0] || !eyes.image[1] || !eyes.view[0] || !eyes.view[1]) {
        return false;
    }
    if (!vr_upscale) {
        vr_upscale = std::make_unique<VrEyeUpscale>();
        if (!vr_upscale->Init(instance.GetDevice(), instance.GetAllocator())) {
            vr_upscale_failed = true;
            return false;
        }
    }
    const std::array<vk::Extent2D, 2> in{eyes.extent[0], eyes.extent[1]};
    if (!vr_upscale->Prepare(in, out, sharpen)) {
        return false;
    }
    vr_warp->EnsureEyeImages(vr_eye_scaled, out[0], out[1]);
    if (!vr_eye_scaled.image[0] || !vr_eye_scaled.image[1]) {
        return false;
    }
    constexpr vk::ImageSubresourceRange color{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    using L = vk::ImageLayout;
    const auto barrier = [&](vk::Image image, L from, L to, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                             vk::AccessFlags2 dst_access) {
        return vk::ImageMemoryBarrier2{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = image,
            .subresourceRange = color,
        };
    };
    // The warped eyes (left for transfer reads by the warp's own barrier) to compute reads; the
    // scaled ones (their last copy into the headset done) to storage writes.
    std::array<vk::ImageMemoryBarrier2, 4> before{};
    for (u32 e = 0; e < 2; ++e) {
        before[e] = barrier(eyes.image[e], L::eTransferSrcOptimal, L::eShaderReadOnlyOptimal,
                            S::eAllTransfer | S::eComputeShader, A::eNone, S::eComputeShader,
                            A::eShaderSampledRead);
        before[2 + e] = barrier(vr_eye_scaled.image[e], L::eUndefined, L::eGeneral,
                                S::eAllTransfer, A::eNone, S::eComputeShader,
                                A::eShaderStorageWrite);
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = before.data()});
    const std::array<VrEyeUpscale::Source, 2> sources{{
        {*eyes.view[0], in[0]},
        {*eyes.view[1], in[1]},
    }};
    const std::array<VrEyeUpscale::Target, 2> targets{{
        {vr_eye_scaled.image[0], *vr_eye_scaled.view[0], {0, 0}, out[0]},
        {vr_eye_scaled.image[1], *vr_eye_scaled.view[1], {0, 0}, out[1]},
    }};
    const bool done =
        vr_upscale->Record(cmdbuf, sources, L::eShaderReadOnlyOptimal, targets, sharpen);
    // Both back for transfer reads (the scaled ones by RecordStereoCopy; the warped ones by the
    // present thread's fallback copy).
    std::array<vk::ImageMemoryBarrier2, 4> after{};
    for (u32 e = 0; e < 2; ++e) {
        after[e] = barrier(eyes.image[e], L::eShaderReadOnlyOptimal, L::eTransferSrcOptimal,
                           S::eComputeShader, A::eNone, S::eAllTransfer, A::eTransferRead);
        after[2 + e] = barrier(vr_eye_scaled.image[e], L::eGeneral, L::eTransferSrcOptimal,
                               S::eComputeShader, A::eShaderStorageWrite, S::eAllTransfer,
                               A::eTransferRead);
        vr_eye_scaled.layout[e] = L::eTransferSrcOptimal;
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = after.data()});
    return done;
}

void Presenter::RecordVrUi(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size) {
    const VR::FrameTag& tag = frame.vr_tag;
    if (!tag.valid || !vr_ui_layer) {
        return;
    }
    auto& stats = vr_ui_stats;
    struct Report {
        VrUiStats& stats;
        VrUiLayer& layer;
        ~Report() {
            if (++stats.flips < 300) {
                return;
            }
            const VrUiLayer::Stats g = layer.TakeStats();
            const VrUiLayer::GpuTimes t = layer.TakeGpuTimes();
            if (stats.ui_frames != 0 || stats.armed != 0 || t.frames != 0) {
                const double armed = std::max<double>(stats.armed, 1.0);
                const double frames = std::max<double>(g.frames, 1.0);
                LOG_INFO(
                    Render_Vulkan,
                    "VR ui: {} flips, {} UI-layer frames (gameplay {}, menus {}, dialogs {}, notes "
                    "{}): {} on the layer, {} with no UI, {} incomplete, {} without the "
                    "composite, {} refused{}, {} with mono eyes{}; per shadowed frame {:.1f} UI "
                    "draws, {:.1f} shadowed, {:.1f} offtarget, {:.1f} backdrop, {:.1f} non-affine, "
                    "{:.1f} dst-alpha; veil in {} frames (darkest {:.3f}, last {:.3f}, T {:.3f} "
                    "{:.3f} "
                    "{:.3f} K {:.3f} {:.3f} {:.3f}; strips T {:.3f} spread {:.3f}, K {:.4f} spread "
                    "{:.4f}); UI px per frame {:.0f} ({:.0f} opaque, {:.0f} "
                    "left on the rest quad, {:.0f} at the band border, {:.0f} with W clipped "
                    "(additive){}){}{}{}",
                    stats.flips, stats.ui_frames, stats.classes[0], stats.classes[1],
                    stats.classes[2], stats.classes[3], stats.layer, stats.empty, stats.incomplete,
                    stats.no_marker, stats.refused,
                    stats.refusal != nullptr
                        ? fmt::format(" ({}{})", stats.refusal,
                                      stats.held != 0
                                          ? fmt::format("; {} menu frames held", stats.held)
                                          : std::string{})
                        : std::string{},
                    stats.mono_eyes,
                    (stats.checked != 0 ? fmt::format(", {} checked", stats.checked)
                                        : std::string{}) +
                        (stats.page_held != 0
                             ? fmt::format(", {} menu frames without the menu held at the VR "
                                           "page's open",
                                           stats.page_held)
                             : std::string{}),
                    stats.ui_draws / armed, stats.shadowed / armed, stats.offtarget / armed,
                    stats.backdrop / armed, stats.nonaffine / armed, stats.dst_alpha / armed,
                    g.veil_frames, g.veil_min, g.veil_last, g.veil_t[0], g.veil_t[1], g.veil_t[2],
                    g.veil_k[0], g.veil_k[1], g.veil_k[2], g.strip_t, g.strip_spread, g.strip_k,
                    g.strip_k_spread, g.content / frames, g.opaque / frames, g.rest / frames,
                    g.border / frames, g.saturated / frames,
                    g.split_frames == 0
                        ? std::string{}
                        : fmt::format("; split: {} gameplay frames, {:.1f} HUD rows and {:.0f} px "
                                      "of other UI in the lower group per frame",
                                      g.split_frames,
                                      static_cast<double>(g.split_rows) /
                                          static_cast<double>(g.split_frames),
                                      static_cast<double>(g.split_out) /
                                          static_cast<double>(g.split_frames)),
                    (g.menu_gone == 0 ? std::string{}
                                      : fmt::format("; {} menu frames without the menu (canvas "
                                                    "left empty)",
                                                    g.menu_gone)) +
                        (g.menu_hud_kept + g.menu_hud_whole + g.menu_hud_empty == 0
                             ? std::string{}
                             : fmt::format("; HUD under the menu canvas: {} frames kept before "
                                           "the menu ({} with its dim taken back out), {} all "
                                           "HUD, {} none; {:.0f} px per frame ({:.0f} px of the "
                                           "clusters left out)",
                                           g.menu_hud_kept, g.menu_hud_veiled, g.menu_hud_whole,
                                           g.menu_hud_empty,
                                           static_cast<double>(g.menu_hud_px) /
                                               std::max<double>(g.menu_hud_kept +
                                                                    g.menu_hud_whole,
                                                                1.0),
                                           static_cast<double>(g.menu_hud_out) /
                                               std::max<double>(g.menu_hud_kept +
                                                                    g.menu_hud_whole,
                                                                1.0))),
                    g.check_n == 0
                        ? std::string{}
                        : fmt::format("; check: {} px, mean err {:.3f} LSB, max {} LSB, {:.4f}% "
                                      "> 3 LSB, {:.4f}% > 8 LSB",
                                      g.check_n,
                                      static_cast<double>(g.check_sum) /
                                          static_cast<double>(g.check_n),
                                      g.check_max,
                                      100.0 * static_cast<double>(g.check_bad) /
                                          static_cast<double>(g.check_n),
                                      100.0 * static_cast<double>(g.check_bad8) /
                                          static_cast<double>(g.check_n)),
                    t.frames == 0
                        ? std::string{}
                        : fmt::format("; GPU composite->flip {:.3f} ms (max {:.3f}), resolve "
                                      "{:.3f} ms (max {:.3f}) over {} frames",
                                      t.ui_ms, t.ui_max_ms, t.resolve_ms, t.resolve_max_ms,
                                      t.frames));
                if (g.marker_frames != 0) {
                    const auto& b = g.marker_box;
                    LOG_INFO(Render_Vulkan,
                             "VR ui: lock-on marker in {} frames ({} without a marker pixel): "
                             "x{:.2f} its size about its centre, x{:.2f} its alpha; newest box "
                             "{}x{} px centred at ({:.1f}, {:.1f}) band px",
                             g.marker_frames, g.marker_empty, g.marker_scale, g.marker_alpha,
                             b[2] - b[0] + 1, b[3] - b[1] + 1, 0.5 * (b[0] + b[2] + 1),
                             0.5 * (b[1] + b[3] + 1));
                }
            }
            stats = {};
        }
    } report{stats, *vr_ui_layer};

    const auto& info = rasterizer->VrUiFrame();
    if (tag.ui_layer != 0 && tag.immersive) {
        ++stats.ui_frames;
        ++stats.classes[std::min<u32>(tag.ui_class, 3)];
        if (tag.ui_layer == 1) {
            VR::NoteUiMarker(info.marker);
        }
        if (!info.marker) {
            ++stats.no_marker;
        } else if (!info.armed) {
            ++stats.refused;
            stats.refusal = info.refused;
        }
    }
    // A menu or dialog frame whose UI was refused at the composite (the Status page's first frame
    // draws no scene target before it) would reach the headset as the game's own image, the whole
    // menu stretched over the view in mono, with the canvas gone for that frame. It is held
    // instead: the last eyes and canvas stay up, at most 4 frames in a row, so a menu that keeps
    // refusing is shown the old way until a frame is captured.
    const bool refused_canvas =
        tag.ui_layer == 1 && tag.immersive && tag.ui_class != 0 && info.marker && !info.armed;
    // A on the Options grid's VR tile: the game draws 1-4 frames of its full menu without the
    // menu (every UI draw the HUD's, or none) before the VR page fades in over it, and the grid
    // blinked out for ~3 XR frames (the play test of 2026-10-09). Within 0.5 s of the A they are
    // held the same way (the last eyes and canvas stay up, at most 4 frames in a row).
    const bool menu_gone_at_page =
        tag.ui_layer == 1 && tag.immersive &&
        tag.ui_class == static_cast<u8>(VR::UiClass::Full) && info.marker && info.armed &&
        info.hud_draws < 0 && VR::Page::Runtime::Get().SecondsSinceRequest() < 0.5;
    if (!refused_canvas && !menu_gone_at_page) {
        vr_ui_held_run = 0;
    } else if (vr_ui_held_run < 4) {
        ++vr_ui_held_run;
        ++(menu_gone_at_page ? stats.page_held : stats.held);
        frame.vr_tag.hold = true;
        if (menu_gone_at_page) {
            vr_ui_layer->MarkFlip(cmdbuf); // nothing of this frame reaches the headset
            return;
        }
    }
    if (!info.armed || !tag.immersive) {
        // Not a UI-layer frame (or one going to the virtual screen, which keeps its UI).
        vr_ui_layer->MarkFlip(cmdbuf);
        return;
    }
    ++stats.armed;
    stats.ui_draws += info.ui_draws;
    stats.shadowed += info.shadowed;
    stats.offtarget += info.offtarget;
    stats.backdrop += info.backdrop;
    stats.nonaffine += info.nonaffine;
    stats.dst_alpha += info.dst_alpha;
    if (info.incomplete) {
        ++stats.incomplete;
        if (NativeStereo::Enabled()) {
            // Keep the complete game image when extraction failed, never a partial duplicate.
            vr_ui_layer->MarkFlip(cmdbuf);
            return;
        }
    }

    // The 16:9 band of a taller render: the game's UI stage.
    const vk::Extent2D x = info.x_extent;
    vk::Rect2D band{{0, 0}, x};
    if (static_cast<float>(x.width) / static_cast<float>(x.height) < 16.0f / 9.0f - 0.02f) {
        const u32 h = static_cast<u32>(std::lround(x.width * 9.0f / 16.0f));
        band = {{0, static_cast<s32>((x.height - h) / 2)}, {x.width, h}};
    }
    // The scene the eyes are made from: the DIBR capture's copy or the UI layer's.
    VrUiLayer::SceneRef scene{};
    if (info.scene_in_warp) {
        const auto warp_scene = vr_warp->Scene();
        scene = {
            .image = warp_scene.image,
            .sampled = warp_scene.raw,
            .storage = warp_scene.storage,
            .decoded = warp_scene.decoded,
            .extent = warp_scene.extent,
            .layout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
    } else {
        scene = vr_ui_layer->Scene();
    }
    const auto& s = VR::GetSettings();
    VrUiLayer::ResolveParams params{};
    params.band = band;
    params.veil = s.ui_veil;
    if (info.mode == 2) {
        // Check only: X against K + T * S; nothing visible changes.
        VideoCore::Image* x_image = rasterizer->VrUiTarget();
        if (x_image == nullptr || !scene.sampled) {
            return;
        }
        x_image->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::AccessFlagBits2::eShaderSampledRead, {}, cmdbuf);
        VideoCore::ImageViewInfo view_info{};
        view_info.format = info.x_format;
        view_info.mapping = info.x_components;
        const auto& view = x_image->FindView(view_info, false);
        params.check = true;
        params.x_final = {.sampled = *view.image_view,
                          .layout = vk::ImageLayout::eShaderReadOnlyOptimal};
        if (vr_ui_layer->Resolve(cmdbuf, nullptr, scene, params)) {
            ++stats.checked;
        }
        return;
    }
    // The HUD band's regions stay out of the rest quad; the dynamic one (the lower group) keeps
    // only its wide elements on the band in gameplay (a lock-on cursor there stays in place), up
    // to the stage's right edge (an area name, VR::Ui::CutRect).
    params.dialogue = tag.ui_class == static_cast<u8>(VR::UiClass::None) && VR::IsNpcSubtitleUp();
    auto hud = VR::Ui::ComputeHudLayout(static_cast<VR::Ui::HudLayout>(s.ui_hud_layout),
                                              s.ui_hud_scale_deg, s.ui_hud_pitch_deg);
    VR::Ui::FitDialogue(hud, params.dialogue);
    const float k = VR::Ui::BandScale(static_cast<float>(band.extent.width));
    // An area name (VR::Ui::StepAreaName on the newest measured gameplay frame, a few frames old;
    // not a fade's or a loading page's rows reaching the stage's edge): this frame's lower cut
    // reaches the stage's bottom (its backing's last rows were a detached strip on the rest
    // quad), its corner goes to the band whole and its dark backing is keyed out. The presenter
    // places the piece by this atlas's flag (UiFrame::area_name).
    {
        const auto measured = vr_ui_layer->LatestContentBox();
        const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        const float dt = vr_ui_area_ns != 0 ? static_cast<float>(now_ns - vr_ui_area_ns) * 1e-9f
                                            : 0.0f;
        vr_ui_area_ns = now_ns;
        float right = 0.0f;
        if (tag.ui_class == static_cast<u8>(VR::UiClass::None) && measured.seq != 0 &&
            measured.cls == static_cast<u8>(VR::UiClass::None) && measured.hud_right != 0) {
            right = static_cast<float>(measured.hud_right) / k;
            // Without its dark strip (VR::IsAreaDimRemoved) a name ends by x ~1830 (1661 in the
            // 2026-10-14 run): a row to the edge is a fade over the whole screen, never a name
            // (it put a crop of the fading screen straight ahead on arrival from a lamp).
            if ((vr_ui_screen_wide.on || VR::IsAreaDimRemoved()) &&
                right >= VR::Ui::AreaNameMaxRightX) {
                right = 0.0f;
            }
            // A lantern warp's departure fades the whole screen out (it reached x 1920 and was
            // taken for an area name, its corner put straight ahead mid-warp).
            if (VR::LampWarpDeparting()) {
                right = 0.0f;
            }
        }
        const bool was_on = vr_ui_area.on;
        if (VR::Ui::StepAreaName(vr_ui_area, right, params.dialogue, dt) != was_on) {
            LOG_INFO(Render_Vulkan, "VR ui: area name {}",
                     vr_ui_area.on
                         ? fmt::format("up (the lower group's HUD to stage x {:.0f}, frame {}): its "
                                       "corner keyed, down to the stage's bottom",
                                       right, measured.seq)
                         : std::string{"gone"});
        }
        params.area_name = vr_ui_area.on && !params.dialogue &&
                           tag.ui_class == static_cast<u8>(VR::UiClass::None);
        params.area_key = !VR::IsAreaDimRemoved();
    }
    const auto band_rect = [&](const VR::Ui::StageRect& r) {
        return VrUiLayer::Region{
            static_cast<s32>(std::floor(r.x0 * k)), static_cast<s32>(std::floor(r.y0 * k)),
            static_cast<s32>(std::ceil(r.x1 * k)), static_cast<s32>(std::ceil(r.y1 * k))};
    };
    for (u32 i = 0; i < hud.count && i < params.cut.size(); ++i) {
        const auto& seg = hud.segment[i];
        params.cut[i] = band_rect(VR::Ui::CutRect(seg, params.area_name));
        if (seg.dynamic) {
            params.dynamic_cut = static_cast<s32>(i);
        } else {
            params.status_cuts |= 1u << i; // never part of a dialog's box
        }
    }
    params.cut_count = std::min<u32>(hud.count, static_cast<u32>(params.cut.size()));
    // The status build-up meter (ui_conditions=view): while the game shows a row, in a gameplay
    // frame or a small dialog's or note's (the belt's pieces show), never a screen-wide one, its
    // rows' rect is cut 3: its pixels go into the atlas's strip, out of the HP cluster's piece and
    // the rest quad, for the presenter's conditions piece (VR::Ui::Conditions). The rows are held
    // at their largest for a moment (the game fills them on its own thread).
    namespace Cond = VR::Ui::Conditions;
    params.strip = Cond::StripWanted(s.ui_conditions, s.ui_hud_layout);
    Cond::BandRegion cond{};
    Cond::Rows cond_rows{};
    if (params.strip) {
        const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        cond_rows = Cond::StepHold(vr_ui_conditions_hold, VR::Conditions::Latest(), now_ns);
        const bool belt_class = tag.ui_class == static_cast<u8>(VR::UiClass::None) ||
                                tag.ui_class == static_cast<u8>(VR::UiClass::Dialog) ||
                                tag.ui_class == static_cast<u8>(VR::UiClass::Note);
        if (cond_rows.n > 0 && belt_class && !vr_ui_screen_wide.on && params.cut_count == 3) {
            cond = Cond::CutRegion(cond_rows, band.extent.width);
            params.cut[3] = {cond.x0, cond.y0, cond.x1, cond.y1};
            params.cut_count = 4;
            params.status_cuts |= 1u << 3; // never part of a dialog's box
            params.conditions = true;
        }
    }
    if (params.conditions != vr_ui_conditions_on) {
        vr_ui_conditions_on = params.conditions;
        static u32 cond_logs = 0;
        if (cond_logs < 200) {
            ++cond_logs;
            if (params.conditions) {
                LOG_INFO(Render_Vulkan,
                         "VR ui: build-up meter: {} row(s) (longest bar {}%), band px {} {} {} {} "
                         "into the strip: its own piece under the view's centre",
                         cond_rows.n, cond_rows.max_len, cond.x0, cond.y0, cond.x1, cond.y1);
            } else {
                LOG_INFO(Render_Vulkan, "VR ui: build-up meter gone");
            }
        }
    }
    params.gameplay = tag.ui_class == static_cast<u8>(VR::UiClass::None);
    params.action_prompt = params.gameplay && VR::IsActionPromptUp();
    params.item_pickup = params.gameplay && VR::IsItemPickupUp();
    params.full_menu = tag.ui_class == static_cast<u8>(VR::UiClass::Full) &&
                       VR::MenuPresentMode() == VR::MenuPresent::Layer;
    constexpr VR::Ui::HudSplit split{};
    params.split_px = {static_cast<u32>(std::lround(split.min_width_px * k)),
                       static_cast<u32>(std::lround(split.gap_px * k)),
                       static_cast<u32>(std::lround(split.dilate_px * k)),
                       static_cast<u32>(std::lround(split.margin_px * k))};
    // Dialogs: the box of the UI in the middle of the screen (not the HUD clusters, not the
    // screen's edges). A full menu cropped to its content (ui_menu_fit): the box over the whole
    // band, the gameplay HUD's footprint (kept by every gameplay frame) left out of the clusters.
    params.footprint = params.gameplay;
    params.menu_box = params.full_menu && s.ui_menu_fit;
    // HP, stamina and the items under a full menu's canvas (ui_menu_hud): the HUD the game drew
    // before the menu over it (the rasterizer kept K and W then).
    params.menu_hud = params.full_menu && s.ui_menu_hud && params.status_cuts != 0;
    params.box = params.menu_box
                     ? VrUiLayer::Region{0, 0, static_cast<s32>(band.extent.width),
                                         static_cast<s32>(band.extent.height)}
                     : band_rect(VR::Ui::DialogBoxRegion);
    // The atlas gets the eyes' gamma, like the world behind it.
    params.gamma = pp_settings.gamma;
    params.seq = tag.seq;
    params.cls = tag.ui_class;
    // The lock-on marker, drawn into its own pair this frame (VrUiIsLockMarker): scaled and
    // dimmed by the factors it was drawn for.
    params.marker_scale = info.marker_scale;
    params.marker_alpha = info.marker_alpha;
    params.marker_over = info.marker_after == 0;
    // Pixels no UI drew on exactly transparent (ui_empty_snap; VR_TUNE ui.empty_snap = 0 / 1).
    params.empty_snap = VR::Tune::Get("ui.empty_snap", s.ui_empty_snap ? 1.0f : 0.0f) > 0.5f;
    // The atlas in the UI swapchain's byte order (the eye swapchains' format).
    params.bgra = openxr_presenter == nullptr ||
                  openxr_presenter->EyeSwapchainFormat() != vk::Format::eR8G8B8A8Srgb;
    const auto atlas = vr_ui_layer->AtlasFor(frame.id, band.extent, params.strip);
    const bool resolved = vr_ui_layer->Resolve(cmdbuf, &atlas, scene, params);
    if (resolved) {
        ++stats.layer;
    } else {
        ++stats.empty;
    }
    const auto snap = vr_ui_layer->HudSnapState();
    const bool menu_hud = resolved && params.menu_hud && snap != VrUiLayer::HudSnap::Empty;
    // The gameplay HUD's draws are learnt from gameplay frames (not a screen-wide one: a fade or
    // the death screen); plain translucent rects only while the HUD alone is up (no prompt, text,
    // talk, pop-up or lock-on: their boxes are drawn like a menu's dim). A menu's frame keeps K
    // and W before its first other draw.
    rasterizer->VrUiCommitHud(params.gameplay && !vr_ui_screen_wide.on, VR::IsPlainHud());
    {
        // Once a menu has shown for a moment (its first frames may draw only the HUD), and when a
        // menu chained to it starts with another draw: where its HUD came from.
        static u32 full_frames = 0;
        static u32 menu_hud_logs = 0;
        static u64 logged_first = 0;
        full_frames = params.full_menu ? full_frames + 1 : 0;
        if (full_frames >= 20 && menu_hud_logs < 60 &&
            (full_frames == 20 ||
             (snap == VrUiLayer::HudSnap::Kept && info.first_other != logged_first))) {
            ++menu_hud_logs;
            logged_first = info.first_other;
            LOG_INFO(Render_Vulkan,
                     "VR ui: menu HUD (ui_menu_hud {}): {}{}", s.ui_menu_hud ? 1 : 0,
                     !params.menu_hud ? std::string{"not shown"}
                     : snap == VrUiLayer::HudSnap::Kept
                         ? fmt::format("K and W kept after the HUD's {} of {} UI colour draws "
                                       "(the menu's first draw {:#018x})",
                                       info.hud_draws, info.shadowed, info.first_other)
                     : snap == VrUiLayer::HudSnap::Whole
                         ? std::string{"every UI draw of the frame is the HUD's"}
                         : std::string{"the menu drew first (no HUD learnt yet): none"},
                     menu_hud ? ", shown under the canvas" : "");
        }
    }
    const auto box = vr_ui_layer->LatestContentBox();
    // Back in VR from the virtual screen (a cinematic, a warp's arrival): the screen-wide picture
    // a fade to black turned on is over, and the HUD band (and an area name's own place) is back
    // at once. Before, it held on ~6 s after a lamp's warp (the arrival's wake-up fade, then the
    // area name's ink at the edges), the name 20..35 deg right. The measures are a few frames old:
    // the fade's last ones may not turn it on again for a moment (not while dead: the death
    // screen is one picture).
    const s64 ui_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const bool immersive = VR::IsImmersive();
    VR::Vitals::Status ui_vit{};
    const bool ui_dead = VR::Vitals::LatestStatus(ui_vit) && ui_vit.ok && ui_vit.max_hp > 0 &&
                         ui_vit.hp <= 0 && ui_now_ns - ui_vit.steady_ns < 5'000'000'000;
    if (immersive && !vr_ui_was_immersive && !ui_dead) {
        if (vr_ui_screen_wide.on) {
            LOG_INFO(Render_Vulkan, "VR ui: back in VR: the screen-wide picture is over, the HUD "
                                    "band again");
        }
        vr_ui_screen_wide = {};
        vr_ui_wide_hold_ns = ui_now_ns + 750'000'000;
    }
    vr_ui_was_immersive = immersive;
    const bool wide_held = !ui_dead && ui_now_ns < vr_ui_wide_hold_ns;
    // A gameplay frame whose UI reaches the screen's edges (the death screen's backdrop and YOU
    // DIED, a loading page) goes whole onto the rest quad, never cut into the belt's pieces. Each
    // measured gameplay frame (a few frames old) steps it once; a menu's frames leave it as is.
    if (!wide_held && box.seq != 0 && box.cls == static_cast<u8>(VR::UiClass::None) &&
        VR::Ui::StepScreenWide(vr_ui_screen_wide, box.seq, box.border, band.extent.width,
                               band.extent.height)) {
        const bool wide = vr_ui_screen_wide.on;
        LOG_INFO(Render_Vulkan,
                 "VR ui: gameplay UI {} the screen's edges ({} of {} border px, frame {}): {}",
                 wide ? "reaches" : "left", box.border,
                 VR::Ui::BorderPixels(band.extent.width, band.extent.height), box.seq,
                 wide ? "shown whole on the rest quad, no HUD band" : "the HUD band again");
    }
    // An area name (lower right) is never part of the screen-wide picture: it keeps its place.
    bool area_name = false;
    if (params.gameplay && vr_ui_screen_wide.on && box.hud_right != 0 && band.extent.width != 0) {
        const float right =
            static_cast<float>(box.hud_right) /
            VR::Ui::BandScale(static_cast<float>(band.extent.width));
        area_name = VR::Ui::AreaNameOverScreenWide(right, params.dialogue, ui_dead);
        if (area_name != vr_ui_area_over_wide) {
            LOG_INFO(Render_Vulkan,
                     "VR ui: {} (the lower group's HUD to stage x {:.0f}, frame {})",
                     area_name ? "an area name over the screen-wide picture: kept in its place"
                               : "no area name over the screen-wide picture any more",
                     right, box.seq);
        }
    }
    vr_ui_area_over_wide = area_name;
    // A lantern warp (VR::PublishLampWarp) leaves and arrives in full VR, without the lamp's own
    // scene before the load: on the first frames after it the game still draws its loading page
    // over the new world, and it went whole onto the rest quad (the 2026-10-14 run: the loading
    // page's item text floating over the Great Bridge, a "mini screen" until the arrival's lamp
    // scene). No UI from the warp's start until that scene has played (at most 6 s after the
    // new player).
    // ... and the game's own lantern box while the VR lamp page answers it (VR::IsLampBoxHidden).
    bool lamp_hide = VR::IsLampBoxHidden();
    if (VR::LampWarpDeparting()) {
        lamp_hide = true;
        vr_ui_lamp_scene = false;
    } else if (VR::LampWarpArrivedWithin(6.0f)) {
        const bool scene = VR::CinematicActive();
        vr_ui_lamp_scene = vr_ui_lamp_scene || scene;
        lamp_hide = lamp_hide || !vr_ui_lamp_scene || scene;
    }
    if (lamp_hide != vr_ui_lamp_hidden) {
        LOG_INFO(Render_Vulkan, "VR ui: {}",
                 lamp_hide ? "hidden for the lantern (the game's own box under the VR lamp page, "
                             "or a warp: its departure and the loading page left after it)"
                           : "back after the lantern");
        vr_ui_lamp_hidden = lamp_hide;
    }
    const bool shown = resolved && !lamp_hide;
    frame.vr_ui = {
        .valid = true,
        .empty = !shown,
        .image = shown ? atlas.image : vk::Image{},
        .view = shown ? atlas.storage : vk::ImageView{},
        .extent = atlas.extent,
        .band = band.extent,
        .render = x,
        .box_valid = box.valid,
        .box = {box.x0, box.y0, box.x1, box.y1},
        .regions = box.regions,
        .box_seq = box.seq,
        .box_class = box.cls,
        .box_content = box.content,
        .box_opaque = box.opaque,
        .opaque_box = box.opaque_box,
        .cut_box = box.cut_box,
        .hud_right = box.hud_right,
        // A delayed loading-border measurement must not put a new prompt on the huge rest quad.
        .screen_wide = params.gameplay && vr_ui_screen_wide.on && !params.action_prompt &&
                       !params.item_pickup && !params.dialogue && !area_name,
        .dialogue = params.dialogue,
        .area_name = shown && params.area_name,
        .menu_hud = menu_hud,
        .conditions = shown && params.conditions,
        .conditions_rect = {cond.x0, cond.y0, cond.x1, cond.y1},
        .strip_top = Cond::StripTop(band.extent.height),
    };
}

void Presenter::RecordVrUiEyes(vk::CommandBuffer cmdbuf, Frame& frame, vk::Extent2D game_size) {
    if (!openxr_presenter || !vr_ui_layer) {
        return;
    }
    if (frame.vr_ui.valid && !frame.vr_stereo && !frame.vr_published) {
        // The eyes are not warped (AER, mono, a DIBR frame that could not be): the scene copy,
        // post-processed exactly like the frame (the frame's own image has the UI in it).
        const auto& info = rasterizer->VrUiFrame();
        VrUiLayer::SceneRef scene{};
        if (info.scene_in_warp) {
            const auto warp_scene = vr_warp->Scene();
            scene.decoded = warp_scene.decoded;
            scene.sampled = warp_scene.raw;
            scene.extent = warp_scene.extent;
        } else {
            scene = vr_ui_layer->Scene();
        }
        const vk::ImageView input = scene.decoded ? scene.decoded : scene.sampled;
        if (input && scene.extent == game_size && frame.width == game_size.width &&
            frame.height == game_size.height) {
            const auto eye = vr_ui_layer->EyeImageFor(frame.id, swapchain.GetSurfaceFormat().format,
                                                      {frame.width, frame.height});
            constexpr vk::ImageSubresourceRange range{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            };
            const vk::ImageMemoryBarrier2 to_attachment{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = eye.image,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_attachment,
            });
            Frame target{};
            target.width = frame.width;
            target.height = frame.height;
            target.image = eye.image;
            target.image_view = eye.view;
            auto scene_settings = pp_settings;
            scene_settings.color_adjust = VR::Color::Get().Push();
            pp_pass.Render(cmdbuf, input, game_size, target, scene_settings);
            // The pass leaves its target in eGeneral (PostProcessingPass::Render's post barrier).
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = eye.image,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_read,
            });
            frame.vr_scene_image = eye.image;
            ++vr_ui_stats.mono_eyes;
        }
    }
    if (frame.vr_published) {
        // The eyes went to the headset in this stream: the UI goes with them.
        openxr_presenter->RecordUiCopy(cmdbuf, frame.vr_ui, frame.vr_tag);
    }
}

} // namespace Vulkan
