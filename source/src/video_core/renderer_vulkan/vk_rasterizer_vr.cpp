#include <utility>
#include "video_core/fast_render.h"
#include "../../../scratchpad/dual-eye-perf/draw_registers.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR additions to the rasterizer: the VR_DEBUG_DRAWS draw log, the DIBR stereo capture
// (stereo_mode 2, renderer_vulkan/vr_stereo_warp.h) and the UI layer's shadow draws
// (renderer_vulkan/vr_ui_layer.h).
//
// DIBR capture. The game renders one centre-eye frame. Its composite (vertex shader
// VR::Settings::hud_after_vs) is the last scene pass: every draw after it in the frame is HUD or
// menu. Right after the composite is recorded, outside a render pass, the scene depth is turned
// into per-eye disparity (prep compute pass) and the pre-HUD scene colour is copied, both into the
// presenter's VrStereoWarp. Everything goes into the game's own command stream, so plain barriers
// order this frame's capture, its warp at the flip (Presenter::PrepareFrame) and the next frame's
// capture. The scene depth is the image most of the frame's depth-writing scene draws went to
// (light-volume quads only test depth); it is captured at the composite, before the HUD draws
// touch the depth/stencil surface. Every refusal leaves the frame without a capture, i.e. mono.
//
// In-frame items (VrInjectInFrame, renderer_vulkan/vr_inframe.h): on every immersive frame (DIBR,
// AER, mono) our items of that frame's camera record are drawn into the scene colour X right
// after the composite, depth-tested and written against a private copy of the scene depth; the
// DIBR capture then reads that copy, so they get their own disparity, and the UI layer's scene
// copy (VrUiArm) is taken after them: they are part of the eyes' scene, never of the UI layer.
// Their coverage goes with the DIBR capture (VrStereoWarp::CaptureItemMask), so the HUD drawn over
// them in the eye images (HudMode::Diff) is applied to them instead of cutting them.
//
// The lock-on marker (vr.ini ui_lock_marker_scale / ui_lock_marker_alpha, VR::Ui::LockMarker):
// the glow Bloodborne draws on the locked-on target is one UI draw from a 1024x1024 BC7 atlas no
// other UI draw samples (probe 2026-10-08, the clinic's scourge beast locked on: the frame's last
// UI draw, after the target's HP bar; vs 0x44150ad28c9 / fs 0xcfb8da5cd3a, 30 indices). While
// the game is locked on, a UI draw sampling that atlas (known by its content, wherever the game
// loaded it) goes into the marker's own pair instead of K and W; the resolve scales and dims it.

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "../../../scratchpad/dual-eye-perf/asymmetric_projection.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/vr/inframe.h"
#include "core/vr/live_tune.h"
#include "core/vr/quick_settings.h"
#include "core/vr/ui_layout.h"
#include "core/vr/ui_lock_marker.h"
#include "core/vr/vr_session.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_compile_trace.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_inframe.h"
#include "video_core/renderer_vulkan/vr_render_taa.h"
#include "video_core/renderer_vulkan/vr_stereo_warp.h"
#include "video_core/renderer_vulkan/vr_temporal_capture.h"
#include "video_core/renderer_vulkan/vr_ui_layer.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

u64 VertexHash(const GraphicsPipeline* pipeline) {
    return pipeline->GuestShaderKey(Shader::LogicalStage::Vertex);
}

// The lock-on marker's atlas by its content: FNV-1a 64 of 64 samples of 64 bytes spread evenly
// over its first MiB (LockMarkerAtlasHash: the v1.09 game's atlas, measured 2026-10-08 from a
// dump of its guest memory, the arena save locked on to the clinic's scourge beast).
constexpr u64 LockMarkerAtlasHash = 0x33D81405692695D6ull;
constexpr u32 LockMarkerAtlasSize = 1024;

u64 Fnv1a(const u8* data, size_t size, u64 h = 0xCBF29CE484222325ull) {
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return h;
}

u64 AtlasHash(VAddr address, u32 size) {
    const u32 span = std::min<u32>(size, 1u << 20);
    const u32 step = span / 64;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < 64; ++i) {
        h = Fnv1a(reinterpret_cast<const u8*>(address + static_cast<VAddr>(i) * step), 64, h);
    }
    return h;
}

// The cheap check of an atlas's identity: 8 of AtlasHash's samples spread over the same span (not
// its first bytes alone: many atlases start with the same transparent blocks, so another atlas
// loaded at a reused address would keep the marker's cached hash).
u64 AtlasHead(VAddr address, u32 size) {
    const u32 span = std::min<u32>(size, 1u << 20);
    const u32 step = span / 8;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < 8; ++i) {
        h = Fnv1a(reinterpret_cast<const u8*>(address + static_cast<VAddr>(i) * step), 64, h);
    }
    return h;
}

// Per atlas address: its content hash, re-taken when its head (AtlasHead) changes (memory reused).
struct AtlasEntry {
    u64 head{};
    u64 hash{};
};
std::unordered_map<VAddr, AtlasEntry> g_marker_atlases;
u32 g_marker_atlas_logs = 24;
// The game's lock-on as the GPU thread sees it, held for a second: the frames the GPU records lag
// the game's update, so the marker's last frame after a release still counts (never a frame of
// the marker at full size while it goes).
std::chrono::steady_clock::time_point g_marker_locked_at{};

// The viewport depth range of the current draw, exactly as UpdateViewportScissorState sets it.
// `zero_to_w`: clip-space z is 0..W (D3D), which the DIBR depth model assumes.
void ViewportDepthRange(const AmdGpu::Regs& regs, float& zmin, float& zmax, bool& zero_to_w) {
    const auto& vp = regs.viewports[0];
    const auto& ctl = regs.viewport_control;
    const float zoffset = ctl.zoffset_enable ? vp.zoffset : 0.0f;
    const float zscale = ctl.zscale_enable ? vp.zscale : 1.0f;
    zero_to_w = regs.clipper_control.clip_space != AmdGpu::ClipSpace::MinusWToW;
    zmin = zero_to_w ? zoffset : zoffset - zscale;
    zmax = zoffset + zscale;
}

// One warning per refusal reason and session (GPU thread only).
void WarnOnce(u32 bit, const char* what) {
    static u32 warned = 0;
    if ((warned & bit) == 0) {
        warned |= bit;
        LOG_WARNING(Render_Vulkan, "VR dibr: {}; this frame (and every frame like it) is mono",
                    what);
    }
}

// What the last depth capture used, for the capture-config log line (GPU thread only).
std::string g_depth_source;
std::string g_depth_draws; // its share of the frame's depth-writing scene draws
bool g_proxy_on = false;   // the last capture stamped the held vial's depth proxy

// The gameplay HUD's draws (Rasterizer::VrUiCommitHud): a draw signature seen in this many
// gameplay frames in a row is the HUD's (the status clusters draw in every frame; a fade or a
// banner of a second or so never gets there), and forgotten after this many gameplay frames
// without it.
constexpr u32 VrHudLearnFrames = 120;
constexpr u64 VrHudForgetFrames = 600;
constexpr size_t VrHudMaxDraws = 4096;

template <typename T>
u32 RawWord(const T& reg) {
    static_assert(sizeof(T) == sizeof(u32));
    u32 raw{};
    std::memcpy(&raw, &reg, sizeof(raw));
    return raw;
}

bool SameAspect(vk::Extent2D a, vk::Extent2D b) {
    const float ra = static_cast<float>(a.width) / static_cast<float>(std::max(a.height, 1u));
    const float rb = static_cast<float>(b.width) / static_cast<float>(std::max(b.height, 1u));
    return std::abs(ra - rb) <= 0.02f * rb;
}

// UI layer: one warning per reason and session (GPU thread only).
void UiWarnOnce(u32 bit, const std::string& what) {
    static u32 warned = 0;
    if ((warned & bit) == 0) {
        warned |= bit;
        LOG_WARNING(Render_Vulkan, "VR ui: {}", what);
    }
}

using BlendFactor = AmdGpu::BlendControl::BlendFactor;
using BlendFunc = AmdGpu::BlendControl::BlendFunc;

bool ReadsDstAlpha(BlendFactor f) {
    return f == BlendFactor::DstAlpha || f == BlendFactor::OneMinusDstAlpha ||
           f == BlendFactor::SrcAlphaSaturate;
}

// The blend's colour result is affine in the destination colour, per channel (D' = A + B * D):
// what the K/W pair reproduces exactly.
bool AffineBlend(const AmdGpu::BlendControl& b, AmdGpu::ColorControl::LogicOp logic_op) {
    if (logic_op != AmdGpu::ColorControl::LogicOp::Copy) {
        return false;
    }
    if (!b.enable) {
        return true;
    }
    if (b.color_func == BlendFunc::Min || b.color_func == BlendFunc::Max) {
        return false;
    }
    return b.color_dst_factor != BlendFactor::DstColor &&
           b.color_dst_factor != BlendFactor::OneMinusDstColor;
}

} // Anonymous namespace

void Rasterizer::VrLogDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                           float vr_shift, float vr_hud) {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& key = pipeline->GetGraphicsKey();
    std::string cb0 = "-";
    if (const auto id = cb_descs[0].first; id && state.num_color_attachments > 0) {
        const auto& image = texture_cache.GetImage(id);
        cb0 = fmt::format("{:#x}/{}/{}/vo={}/s{}", image.info.guest_address, image.image_uid,
                          vk::to_string(image.backing->image.image_ci.format),
                          image.usage.vo_surface != 0, image.backing->num_samples);
    }
    std::string db = "-";
    if (const auto id = db_desc.first; id) {
        const auto& image = texture_cache.GetImage(id);
        db = fmt::format("{:#x}/{}/{}/{}x{}/s{} dbw={} dclr={} sten={}", image.info.guest_address,
                         image.image_uid, vk::to_string(image.backing->image.image_ci.format),
                         image.info.size.width, image.info.size.height, image.backing->num_samples,
                         db_desc.second.view_info.is_storage,
                         regs.depth_render_control.depth_clear_enable != 0,
                         regs.depth_control.stencil_enable != 0);
    }
    float zmin{}, zmax{};
    bool zero_to_w{};
    ViewportDepthRange(regs, zmin, zmax, zero_to_w);
    const auto& blend = key.blend_controls[0];
    // The UI layer's questions about each draw: the alpha blend, the colour write mask, the depth
    // and stencil test and writes, whether it reads its own target (feedback loop) and which
    // images it samples.
    const auto& dc = regs.depth_control;
    const auto& sc = regs.stencil_control;
    const std::string zt = dc.depth_enable ? fmt::format("{}{}", static_cast<u32>(dc.depth_func),
                                                         dc.depth_write_enable ? "w" : "")
                                           : std::string{"-"};
    const std::string st =
        dc.stencil_enable
            ? fmt::format("{}/{}/{}/{}/wm{:#x}", static_cast<u32>(dc.stencil_ref_func),
                          static_cast<u32>(sc.stencil_fail_front),
                          static_cast<u32>(sc.stencil_zpass_front),
                          static_cast<u32>(sc.stencil_zfail_front),
                          regs.stencil_ref_front.stencil_write_mask)
            : std::string{"-"};
    std::string tex;
    for (const auto id : bound_images) {
        if (tex.size() > 80) {
            tex += "+";
            break;
        }
        const auto& image = texture_cache.GetImage(id);
        tex += fmt::format("{}{:#x}", tex.empty() ? "" : ",", image.info.guest_address);
    }
    LOG_INFO(Render_Vulkan,
             "VRDRAW hud={} shift={} clipdis={} depth={} n={} prim={} blend={} rt={}x{} xs={} "
             "xo={} ys={} yo={} vs={:#x} fs={:#x} cb0={} db={} z=[{},{}] cs={} bl={}/{}/{} "
             "a={}/{}/{}/{} wm={:#x} zt={} st={} tx={} tex={} ui={} lop={}",
             vr_hud, vr_shift, regs.IsClipDisabled(), state.depth_stencil_attachment.has_depth,
             regs.num_indices, static_cast<u32>(regs.primitive_type), blend.enable != 0,
             state.width, state.height, push_data.xscale, push_data.xoffset, push_data.yscale,
             push_data.yoffset, key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
             key.stage_hashes[u32(Shader::LogicalStage::Fragment)], cb0, db, zmin, zmax,
             zero_to_w ? "0..W" : "-W..W", static_cast<u32>(blend.color_src_factor),
             static_cast<u32>(blend.color_dst_factor), static_cast<u32>(blend.color_func),
             blend.separate_alpha_blend, static_cast<u32>(blend.alpha_src_factor),
             static_cast<u32>(blend.alpha_dst_factor), static_cast<u32>(blend.alpha_func),
             static_cast<u32>(key.write_masks[0]), zt, st, attachment_feedback_loop ? 1 : 0,
             tex.empty() ? std::string{"-"} : tex, VrUiLogTag(), static_cast<u32>(key.logic_op));
}

// Every draw before the composite: its colour target is remembered by size, and a scene draw that
// writes depth (not a clear, not a screen-space rect) into an image of the frame's aspect (not a
// shadow map) makes that image a candidate for the frame's scene depth.
void Rasterizer::VrTrackTargets(const GraphicsPipeline* pipeline, const RenderState& state,
                                bool depth) {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    if (vr_dibr.draws++ == 0) {
        // The camera update the frame is captured and submitted with (VrCaptureDepth): the one
        // the frame is tagged with (taken by VR::StereoForDraw at the same first draw).
        vr_dibr.first_draw_record = VR::FirstDrawRecord();
        if (vr_dibr.first_draw_record == 0) {
            vr_dibr.first_draw_record = VR::DrawFrameRecord().id;
        }
    }
    if (const auto color_id = cb_descs[0].first;
        color_id && state.num_color_attachments > 0 &&
        cb_descs[0].second.view_info.range.base.layer == 0) {
        const auto& image = texture_cache.GetImage(color_id);
        const vk::Extent2D extent{image.info.size.width, image.info.size.height};
        VrColorTarget* slot = nullptr;
        for (auto& target : vr_dibr.color) {
            if (target.id && target.extent == extent) {
                slot = &target;
                break;
            }
        }
        if (slot == nullptr) {
            slot = &*std::ranges::min_element(vr_dibr.color, {}, &VrColorTarget::last_draw);
        }
        // The fragment output was swizzled into the image's channels (CB component swap): the
        // inverse swizzle reads the true colour back.
        const auto swizzle = pipeline->GetGraphicsKey().color_buffers[0].swizzle;
        *slot = {.id = color_id,
                 .uid = image.image_uid,
                 .extent = extent,
                 .components = LiverpoolToVK::ComponentMapping(swizzle.Inverse()),
                 .last_draw = vr_dibr.draws};
    }
    const auto image_id = db_desc.first;
    if (!depth || !image_id || !state.depth_stencil_attachment.has_depth ||
        !db_desc.second.view_info.is_storage || regs.depth_render_control.depth_clear_enable ||
        regs.IsClipDisabled()) {
        return;
    }
    const auto& image = texture_cache.GetImage(image_id);
    const vk::Extent2D depth_extent{image.info.size.width, image.info.size.height};
    const float main_aspect = VR::MainAspect();
    const float aspect =
        static_cast<float>(depth_extent.width) / static_cast<float>(depth_extent.height);
    if (std::abs(aspect - main_aspect) > 0.02f * main_aspect) {
        return; // shadow maps, reflection probes, atlases
    }
    float zmin{}, zmax{};
    bool zero_to_w{};
    ViewportDepthRange(regs, zmin, zmax, zero_to_w);
    VrDepthCandidate* slot = nullptr;
    for (auto& candidate : vr_dibr.depth) {
        if (candidate.draws != 0 && candidate.uid == image.image_uid) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &*std::ranges::min_element(vr_dibr.depth, {}, &VrDepthCandidate::draws);
        *slot = {.id = image_id, .uid = image.image_uid};
        slot->zmin = zmin;
        slot->zmax = zmax;
        slot->zero_to_w = zero_to_w;
    } else if (slot->zmin != zmin || slot->zmax != zmax || slot->zero_to_w != zero_to_w) {
        // A pass with its own depth range (e.g. a compressed first-person weapon range) on the
        // same depth image: one disparity mapping cannot serve both.
        WarnOnce(1u << 0, "scene draws use different viewport depth ranges on one depth image");
        slot->zero_to_w = false;
    }
    ++slot->draws;
}

void Rasterizer::VrBeforeDraw(const GraphicsPipeline* pipeline) {
    if (vr_dibr.captured || vr_dibr.depth_done || !VR::DibrFrameActive() ||
        VertexHash(pipeline) != VR::GetSettings().hud_after_vs) {
        return;
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    if (!(regs.depth_control.depth_enable && regs.depth_control.depth_write_enable)) {
        return; // the usual case: the composite leaves the scene depth alone (captured after it)
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO(Render_Vulkan, "VR dibr: the composite writes depth; depth captured before it");
    }
    vr_dibr.depth_done = VrCaptureDepth({});
}

// Scene depth -> disparity (prep pass) from the best depth candidate, for a `scene`-sized
// colour capture (the depth's own size when `scene` is 0x0). False: nothing captured.
bool Rasterizer::VrCaptureDepth(vk::Extent2D scene) {
    if (NativeStereo::Enabled()) return false;
    // The camera update the frame was built with: the one its first draw pointed at (the oldest
    // flip not reached yet, else the newest update). By the time the composite is reached the
    // newest update is often already the next frame's (Game:Main places the next camera while
    // GXRenderThread still builds this frame): measured in the mock with VR_LAG_TEST=4, the
    // composite's update was one too new on 10-34% of the frames and the flip's tag on 0-100%
    // (depending on the session), the first draw's on none. The disparity and the submitted pose
    // are this update's.
    VR::CameraRecord rec = VR::DrawFrameRecord();
    vr_dibr.composite_record = rec.id;
    if (vr_dibr.first_draw_record != 0 && vr_dibr.first_draw_record != rec.id) {
        VR::CameraRecord first{};
        if (VR::GetRecord(vr_dibr.first_draw_record, first) && first.has_projection) {
            rec = first;
        }
    }
    if (rec.menu) {
        return false; // the frozen menu view: this frame goes to the menu panel
    }
    if (!rec.dibr || !rec.has_projection) {
        WarnOnce(1u << 1, "the frame's camera record is not a stereo_mode 2 render (or has no "
                          "projection)");
        return false;
    }
    const VrDepthCandidate* best = nullptr;
    for (const auto& candidate : vr_dibr.depth) {
        if (candidate.draws != 0 && (best == nullptr || candidate.draws > best->draws)) {
            best = &candidate;
        }
    }
    VideoCore::Image* depth = best ? texture_cache.TryGetImage(best->id, best->uid) : nullptr;
    if (depth == nullptr) {
        WarnOnce(1u << 2, "no depth-writing scene draw before the composite");
        return false;
    }
    if (!best->zero_to_w || !(best->zmax > best->zmin)) {
        WarnOnce(1u << 3, "the scene depth is not in a 0..W clip space with one depth range");
        return false;
    }
    if (depth->backing->num_samples != 1) {
        WarnOnce(1u << 4, "the scene depth is multisampled");
        return false;
    }
    const vk::Extent2D depth_extent{depth->info.size.width, depth->info.size.height};
    if (scene.width == 0 || scene.height == 0) {
        scene = depth_extent;
    }

    const auto& s = VR::GetSettings();
    VrStereo::CameraParams camera{
        .near_z = rec.near_z,
        .far_z = rec.far_z,
        .vfov = rec.proj_vfov,
        .aspect = rec.proj_aspect > 0.0f
                      ? rec.proj_aspect
                      : static_cast<float>(scene.width) / static_cast<float>(scene.height),
        .half_ipd = 0.5f * rec.ipd * rec.world_scale * s.dibr_baseline_scale,
        .units_per_metre = rec.world_scale,
        .depth_min = best->zmin,
        .depth_max = best->zmax,
    };
    if (!VrStereo::IsCameraValid(camera)) {
        WarnOnce(1u << 5, "the frame's projection is not a valid camera");
        return false;
    }
    // A padded depth image is read 1:1 from its top-left corner; another size of the same
    // aspect (a reduced-resolution depth) is read with nearest sampling.
    vk::Extent2D read_extent = depth_extent;
    if (depth_extent.width >= scene.width && depth_extent.height >= scene.height &&
        depth_extent.width <= scene.width + scene.width / 16 &&
        depth_extent.height <= scene.height + scene.height / 16) {
        read_extent = scene;
    } else if (!SameAspect(depth_extent, scene)) {
        WarnOnce(1u << 6, "the scene depth does not match the composite's size or aspect");
        return false;
    }

    scheduler.EndRendering();
    // The in-frame items' private depth copy (scene depth + ours) when they were drawn; else the
    // scene depth itself.
    vk::ImageView depth_view{};
    if (vr_dibr.inframe && vr_inframe) {
        depth_view = vr_inframe->DepthView();
    } else {
        depth->Transit(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                       vk::AccessFlagBits2::eShaderRead, {});
        VideoCore::ImageViewInfo view_info{};
        view_info.format = vk::Format::eR32Sfloat; // the view swaps to the depth format and aspect
        depth_view = *depth->FindView(view_info, false).image_view; // never the backing samples
    }
    VrStereoWarp::Settings settings{};
    settings.knee_m = s.dibr_knee_m;
    settings.min_depth_m = s.dibr_near_clamp_m;
    // The game's vial in the hunter's hand (core/vr/bloodborne_held.h) writes no depth: its
    // capsule, placed by the render-camera hook for this update, is stamped instead.
    if (s.vial_dibr_proxy && rec.has_held_proxy) {
        settings.proxy = {
            .valid = true,
            .a = {rec.held_a.x, rec.held_a.y, rec.held_a.z},
            .b = {rec.held_b.x, rec.held_b.y, rec.held_b.z},
            .radius = rec.held_radius,
        };
    }
    // aa_mode=taa: the resolve's stable depth stands in for the game's jittered depth (our
    // in-frame items stay where the private copy is nearer than the game's).
    VrStereoWarp::StableDepthInput stable{};
    const bool use_stable = vr_dibr.taa_resolved && s.taa_stable_depth && vr_render_taa &&
                            vr_render_taa->StableDepthView() &&
                            vr_render_taa->Extent() == read_extent;
    if (use_stable) {
        depth->Transit(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                       vk::AccessFlagBits2::eShaderRead, {});
        VideoCore::ImageViewInfo raw_info{};
        raw_info.format = vk::Format::eR32Sfloat;
        stable = {
            .stable = vr_render_taa->StableDepthView(),
            .raw = *depth->FindView(raw_info, false).image_view,
            .raw_layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        };
    }
    vr_warp->BeginFrame(rec.id);
    if (!vr_warp->CaptureDepth(scheduler.CommandBuffer(), depth_view,
                               vk::ImageLayout::eDepthStencilReadOnlyOptimal, read_extent, scene,
                               camera, settings, use_stable ? &stable : nullptr)) {
        return false;
    }
    if (const auto& proxy = vr_warp->GetCaptureInfo().proxy; (proxy.on > 0.0f) != g_proxy_on) {
        // Logged on every change (arming and putting the vial away), the first 40.
        g_proxy_on = proxy.on > 0.0f;
        static u32 proxy_logs = 0;
        if (proxy_logs < 40) {
            ++proxy_logs;
            if (g_proxy_on) {
                LOG_INFO(Render_Vulkan,
                         "VR dibr: held vial depth proxy on: px ({:.0f},{:.0f}) - ({:.0f},{:.0f}) "
                         "at {:.3f} - {:.3f} units, radius {:.1f} px, D {:.1f} px",
                         proxy.ax, proxy.ay, proxy.bx, proxy.by, 1.0f / proxy.inv_za,
                         1.0f / proxy.inv_zb, proxy.r_fx * proxy.inv_za,
                         VrStereo::ProxyDisparity(0.5f * (proxy.ax + proxy.bx),
                                                  0.5f * (proxy.ay + proxy.by), proxy,
                                                  vr_warp->GetCaptureInfo().disparity));
            } else {
                LOG_INFO(Render_Vulkan, "VR dibr: held vial depth proxy off");
            }
        }
    }
    u32 total_draws = 0;
    for (const auto& candidate : vr_dibr.depth) {
        total_draws += candidate.draws;
    }
    vr_dibr.depth_extent = depth_extent;
    g_depth_source =
        fmt::format("depth {}x{} {} s{}, z [{}, {}]", depth_extent.width, depth_extent.height,
                    vk::to_string(depth->backing->image.image_ci.format),
                    depth->backing->num_samples, best->zmin, best->zmax);
    g_depth_draws = fmt::format("{} of {}", best->draws, total_draws);
    return true;
}

// VR_CAMERA_MATCH and VR_UI_TIMING, read once at start-up: VrMatchCamera and VrAfterDraw run per
// draw, and a function-local static's guard is a thread-local read per call.
static const bool g_camera_match = [] {
    const char* value = std::getenv("VR_CAMERA_MATCH");
    return value == nullptr || std::string_view{value} != "0";
}();
static const bool g_ui_timing = std::getenv("VR_UI_TIMING") != nullptr;

void Rasterizer::VrMatchCamera(const GraphicsPipeline* pipeline, const RenderState& state) {
    const bool enabled = g_camera_match;
    // The first few main-view draws (the scene's own depth, the render's aspect) until it is found.
    if (!enabled || vr_dibr.matched_record != 0 || vr_dibr.match_tries >= 4 ||
        !state.depth_stencil_attachment.has_depth || !db_desc.first) {
        return;
    }
    const auto& depth = texture_cache.GetImage(db_desc.first);
    const float main_aspect = VR::MainAspect();
    const float aspect =
        static_cast<float>(depth.info.size.width) / static_cast<float>(depth.info.size.height);
    if (std::abs(aspect - main_aspect) > 0.02f * main_aspect ||
        std::as_const(ScratchDraw::Registers(liverpool)).IsClipDisabled()) {
        return;
    }
    ++vr_dibr.match_tries;
    const u64 first = VR::FirstDrawRecord();
    if (first == 0) {
        return;
    }
    // The candidates: the first draw's update and the six before it. Each is known by the exact
    // floats the render-camera hook wrote (its position) and the view direction it wrote (game
    // axes).
    struct Candidate {
        u64 id;
        std::array<u32, 3> position;
        std::array<float, 3> forward;
    };
    std::array<Candidate, 8> candidates{};
    u32 count = 0;
    // Newest first: the frame's constants were built before its first draw, so with equal poses
    // (standing still) the newest equal one is taken.
    for (u64 id = first; id + 7 > first && id > 0 && count < candidates.size(); --id) {
        VR::CameraRecord r{};
        if (!VR::GetRecord(id, r) || !r.has_render_position || !r.driven) {
            continue;
        }
        Candidate& c = candidates[count++];
        c.id = id;
        std::memcpy(c.position.data(), &r.render_position, sizeof(c.position));
        const float yaw = r.base_yaw - r.head_yaw;
        const float pitch = -r.head_pitch;
        c.forward = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                     std::cos(yaw) * std::cos(pitch)};
    }
    if (count == 0) {
        return;
    }
    // The vertex shader's small constant buffers (Bloodborne: an 864-byte scene block, also bound
    // to the pixel shader, holds the camera position as a matrix column and the view direction as
    // rows and a column).
    const Shader::Info* vs = nullptr;
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->l_stage == Shader::LogicalStage::Vertex) {
            vs = stage;
            break;
        }
    }
    if (vs == nullptr) {
        return;
    }
    u64 found = 0;
    u32 matches = 0;
    for (const auto& desc : vs->buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        const auto sharp = desc.GetSharp(*vs);
        const u32 size = static_cast<u32>(sharp.GetSize());
        if (size < 48 || size > 2048 || sharp.base_address < 0x10000) {
            continue;
        }
        const u32 n = size / 4;
        std::array<u32, 512> words{};
        std::memcpy(words.data(), reinterpret_cast<const void*>(sharp.base_address), n * 4);
        const auto at = [&](u32 i) {
            float f;
            std::memcpy(&f, &words[i], sizeof(f));
            return f;
        };
        for (u32 k = 0; k < count; ++k) {
            const Candidate& c = candidates[k];
            bool position = false;
            bool forward = false;
            for (u32 i = 0; i + 2 < n && !(position && forward); ++i) {
                for (const u32 stride : {1u, 4u}) {
                    if (i + 2 * stride >= n) {
                        continue;
                    }
                    position = position ||
                               (words[i] == c.position[0] && words[i + stride] == c.position[1] &&
                                words[i + 2 * stride] == c.position[2]);
                    forward = forward || (std::abs(at(i) - c.forward[0]) +
                                              std::abs(at(i + stride) - c.forward[1]) +
                                              std::abs(at(i + 2 * stride) - c.forward[2]) <
                                          3e-5f);
                }
            }
            if (position && forward) {
                ++matches;
                if (found == 0) {
                    found = c.id;
                }
            }
        }
        if (found != 0) {
            break;
        }
    }
    auto& stats = vr_camera_match;
    if (found == 0) {
        if (vr_dibr.match_tries == 4) {
            ++stats.unmatched;
        }
    } else {
        vr_dibr.matched_record = found;
        vr_dibr.first_draw_record = found;
        VR::SetFrameRecord(found);
        ++stats.matched;
        const s64 offset = static_cast<s64>(found) - static_cast<s64>(first);
        ++stats.offsets[std::clamp<s64>(offset + 3, 0, 5)];
        stats.ambiguous += matches > 1 ? 1 : 0;
    }
    if (stats.matched + stats.unmatched >= 600) {
        LOG_INFO(Render_Vulkan,
                 "VR camera match: {} frames found the camera update their draws use in their "
                 "constants, {} did not (first draw's kept); the update used vs the first draw's: "
                 "-3 or older {}, -2 {}, -1 {}, same {}, +1 {}, +2 {}; {} with equal poses",
                 stats.matched, stats.unmatched, stats.offsets[0], stats.offsets[1],
                 stats.offsets[2], stats.offsets[3], stats.offsets[4], stats.offsets[5],
                 stats.ambiguous);
        stats = {};
    }
}

void Rasterizer::VrAfterDraw(const GraphicsPipeline* pipeline, const RenderState& state) {
    if (vr_ui.marker) {
        return; // the UI phase: nothing more to track
    }
    // S1 (FAST bit 51, vk_rasterizer_temporal.cpp): the composite was just drawn into X; the
    // reconstruction comes before our in-frame items and the UI layer's scene copy below.
    if (vr_temporal && vr_temporal->s1.pending &&
        VertexHash(pipeline) == VR::GetSettings().hud_after_vs) {
        VrS1Reconstruct();
    }
    if (VR::IsImmersive()) {
        VrMatchCamera(pipeline, state);
    }
    const bool dibr = !NativeStereo::Enabled() && !vr_dibr.captured && VR::DibrFrameActive();
    const s32 ui_mode = vr_ui_layer ? VR::UiLayerFrameMode() : 0;
    const bool time_it = g_ui_timing && vr_ui_layer && VR::IsImmersive();
    bool inframe = false;
    if (!dibr && VR::IsImmersive()) {
        // AER and mono frames track their targets for in-frame items too: decided once, at the
        // frame's first draw (the render-camera hook submitted the frame's list before it).
        if (!vr_dibr.inframe_checked) {
            vr_dibr.inframe_checked = true;
            vr_dibr.inframe_wanted =
                VR::Quick::GetQuickSettings().inframe && VR::InFrame::Has(VR::FirstDrawRecord());
        }
        inframe = vr_dibr.inframe_wanted;
    }
    // S1 (bit 51) finds the composite target the way the DIBR/UI capture does: the last colour
    // target of the scene's size before the first HUD draw (VrS1Reconstruct).
    const bool s1_track = vr_temporal && vr_temporal->s1.on;
    if (!dibr && !inframe && ui_mode == 0 && !time_it && !s1_track) {
        return;
    }
    // The composite's own colour target counts as the last one of its size (depth candidates
    // only for DIBR and in-frame items: the UI layer needs the colour target alone).
    if (dibr || inframe || ui_mode != 0 || s1_track) {
        VrTrackTargets(pipeline, state, dibr || inframe);
    }
    if (VertexHash(pipeline) != VR::GetSettings().hud_after_vs) {
        return;
    }
    vr_ui.marker = true;     // once per frame
    vr_dibr.captured = true; // once per frame, captured or not
    // Our in-frame items go in first: the DIBR capture and the UI layer's scene copy below take
    // them with the scene (never into the UI layer).
    if (!vr_dibr.depth_done && (dibr || inframe)) {
        VrInjectInFrame();
    }
    if (dibr) {
        VrDibrCaptureAtMarker();
    }
    if (time_it) {
        scheduler.EndRendering();
        vr_ui_layer->MarkComposite(scheduler.CommandBuffer());
    }
    if (ui_mode != 0) {
        VrUiArm(ui_mode);
    }
}

void Rasterizer::VrDibrCaptureAtMarker() {
    // Depth first: X is the last colour target of the scene depth's size (the HUD is drawn into
    // it next).
    if (!vr_dibr.depth_done && !VrCaptureDepth({})) {
        return;
    }
    const VrColorTarget* color = nullptr;
    for (const auto& target : vr_dibr.color) {
        if (target.id && target.extent == vr_dibr.depth_extent &&
            (color == nullptr || target.last_draw > color->last_draw)) {
            color = &target;
        }
    }
    const VideoCore::Image* x_image =
        color ? texture_cache.TryGetImage(color->id, color->uid) : nullptr;
    if (x_image == nullptr) {
        vr_warp->BeginFrame(0);
        WarnOnce(1u << 7, "no colour target of the scene depth's size before the composite");
        return;
    }
    const auto& x = *x_image;
    const auto x_id = color->id;
    const vk::ComponentMapping x_components = color->components;
    const vk::Extent2D x_extent{x.info.size.width, x.info.size.height};
    const vk::Format x_format = x.backing->image.image_ci.format;
    const u32 x_samples = x.backing->num_samples;
    const u64 x_uid = x.image_uid;
    const bool x_vo = x.usage.vo_surface != 0;

    scheduler.EndRendering();
    // The texture cache may have moved X while the depth was captured (it never does in practice;
    // the uid check at the flip catches it either way).
    auto* target = texture_cache.TryGetImage(x_id, x_uid);
    if (target == nullptr) {
        WarnOnce(1u << 8, "the composite target went away during the capture");
        return;
    }
    target->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    if (!vr_warp->CaptureColor(scheduler.CommandBuffer(), target->GetImage(),
                               vk::ImageLayout::eTransferSrcOptimal, x_format, x_extent,
                               static_cast<vk::SampleCountFlagBits>(x_samples), x_components)) {
        return;
    }
    if (!vr_warp->HasCapture()) {
        WarnOnce(1u << 9, "the composite target and the scene depth differ in size");
        return;
    }
    // Our in-frame items' coverage: the HUD over them is applied to them, never cuts them.
    if (vr_dibr.inframe && vr_inframe && vr_inframe->CoverExtent() == x_extent) {
        vr_warp->CaptureItemMask(scheduler.CommandBuffer(), vr_inframe->CoverImage(), x_extent);
    }
    vr_dibr.color_id = x_id;
    vr_dibr.color_uid = x_uid;
    vr_dibr.color_components = x_components;

    const auto& info = vr_warp->GetCaptureInfo();
    const std::string config = fmt::format(
        "X {}x{} {} s{} vo={} read as {}{}{}{}; {}; camera n {} f {} vfov {:.2f} deg aspect "
        "{:.4f} half ipd {:.4f} "
        "(units/m {}); K {:.3f} px*unit, reach {:.1f} px, knee {:.1f} px, floor {:.1f} px",
        x_extent.width, x_extent.height, vk::to_string(x_format), x_samples, x_vo,
        vk::to_string(x_components.r), vk::to_string(x_components.g), vk::to_string(x_components.b),
        vk::to_string(x_components.a), g_depth_source, info.camera.near_z, info.camera.far_z,
        info.camera.vfov * 57.2957795f, info.camera.aspect, info.camera.half_ipd,
        info.camera.units_per_metre, info.disparity.k_px, info.reach_px, info.disparity.knee_px,
        info.disparity.floor_px);
    static std::string logged_config;
    static u32 config_logs = 0;
    // The layout of the capture (formats, sizes, the depth source) is logged when it changes;
    // the IPD and FOV may change continuously, so only the first 8 changes are logged.
    if (config != logged_config && config_logs < 8) {
        ++config_logs;
        logged_config = config;
        LOG_INFO(Render_Vulkan, "VR dibr: capture {} ({} depth-writing draws on the depth)", config,
                 g_depth_draws);
    }
}

bool Rasterizer::VrInjectInFrame() {
    vr_dibr.inframe = false;
    if (!VR::Quick::GetQuickSettings().inframe) {
        return false;
    }
    if (!vr_inframe) {
        static bool created = false;
        if (created) {
            return false;
        }
        created = true;
        vr_inframe = std::make_unique<VrInFrame>(instance, scheduler);
    }
    if (!vr_inframe->Active()) {
        return false;
    }
    vr_inframe->LogStats();
    // The camera update the frame was built with (as VrCaptureDepth picks it).
    VR::CameraRecord rec = VR::DrawFrameRecord();
    if (vr_dibr.first_draw_record != 0 && vr_dibr.first_draw_record != rec.id) {
        VR::CameraRecord first{};
        if (VR::GetRecord(vr_dibr.first_draw_record, first) && first.has_projection) {
            rec = first;
        }
    }
    if (rec.menu || !rec.driven || !rec.has_projection) {
        return false;
    }
    const VrDepthCandidate* best = nullptr;
    for (const auto& candidate : vr_dibr.depth) {
        if (candidate.draws != 0 && (best == nullptr || candidate.draws > best->draws)) {
            best = &candidate;
        }
    }
    VideoCore::Image* depth = best ? texture_cache.TryGetImage(best->id, best->uid) : nullptr;
    if (depth == nullptr || !best->zero_to_w || depth->backing->num_samples != 1) {
        return false;
    }
    const vk::Extent2D depth_extent{depth->info.size.width, depth->info.size.height};
    const VrColorTarget* color = nullptr;
    for (const auto& target : vr_dibr.color) {
        if (target.id && target.extent == depth_extent &&
            (color == nullptr || target.last_draw > color->last_draw)) {
            color = &target;
        }
    }
    VideoCore::Image* x = color ? texture_cache.TryGetImage(color->id, color->uid) : nullptr;
    if (x == nullptr || x->backing->num_samples != 1) {
        return false;
    }
    // The game writes X through its colour buffer's component swap (Bloodborne: red and blue
    // swapped, read back as B G R): our colours go through an sRGB view of the opposite byte
    // order then, so they land in X exactly as the game's own.
    const vk::Format x_format = x->backing->image.image_ci.format;
    const vk::ComponentMapping& c = color->components;
    const auto is = [](vk::ComponentSwizzle s, vk::ComponentSwizzle channel) {
        return s == vk::ComponentSwizzle::eIdentity || s == channel;
    };
    const bool identity = is(c.r, vk::ComponentSwizzle::eR) && is(c.g, vk::ComponentSwizzle::eG) &&
                          is(c.b, vk::ComponentSwizzle::eB);
    const bool swapped = c.r == vk::ComponentSwizzle::eB && is(c.g, vk::ComponentSwizzle::eG) &&
                         c.b == vk::ComponentSwizzle::eR;
    const bool rgba = x_format == vk::Format::eR8G8B8A8Unorm || x_format == vk::Format::eR8G8B8A8Srgb;
    const bool bgra = x_format == vk::Format::eB8G8R8A8Unorm || x_format == vk::Format::eB8G8R8A8Srgb;
    vk::Format srgb = vk::Format::eUndefined;
    if ((rgba && identity) || (bgra && swapped)) {
        srgb = vk::Format::eR8G8B8A8Srgb;
    } else if ((bgra && identity) || (rgba && swapped)) {
        srgb = vk::Format::eB8G8R8A8Srgb;
    }
    const vk::Format depth_format = depth->backing->image.image_ci.format;
    if (srgb == vk::Format::eUndefined) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARNING(Render_Vulkan,
                        "VR inframe: the scene colour is {} (components {} {} {}): no in-frame "
                        "items",
                        vk::to_string(x_format), vk::to_string(c.r), vk::to_string(c.g),
                        vk::to_string(c.b));
        }
        return false;
    }
    const bool stereo = NativeStereo::Enabled() && x->native_stereo;
    if (stereo && !depth->native_stereo) {
        return false; // no matching right-eye depth: do not claim the wrist was rendered
    }
    if (stereo && !vr_inframe_right) {
        vr_inframe_right = std::make_unique<VrInFrame>(instance, scheduler);
    }
    if (stereo && !vr_inframe_right->Active()) {
        return false;
    }
    VR::InFrame::List list{};
    const float aspect = rec.proj_aspect > 0.0f ? rec.proj_aspect
                             : static_cast<float>(depth_extent.width) / depth_extent.height;
    std::array<NativeProjection::Transform, 2> projections{};
    if (stereo && NativeStereo::AsymmetricProjection()) {
        const float tan_y = std::tan(rec.proj_vfov * 0.5f);
        for (u32 eye = 0; eye < 2; ++eye) {
            const auto& fov = rec.eye_fov[eye];
            projections[eye] = NativeProjection::Make(
                {fov.angle_left, fov.angle_right, fov.angle_up, fov.angle_down},
                tan_y * aspect, tan_y);
        }
        if (!projections[0].valid || !projections[1].valid) {
            projections = {}; // same all-or-neither fallback as the scene camera
        }
    }
    const float eye_half_width = stereo ? 0.5f * NativeStereo::Ipd() * rec.world_scale : 0.0f;
    float horizontal_extent = 1.0f, vertical_extent = 1.0f;
    for (const auto& projection : projections) {
        if (projection.valid) {
            horizontal_extent = std::max(horizontal_extent,
                (1.0f + std::abs(projection.x_offset)) / projection.x_scale);
            vertical_extent = std::max(vertical_extent,
                (1.0f + std::abs(projection.y_offset)) / projection.y_scale);
        }
    }
    // Whole off-screen holsters need neither the light probe nor a private depth copy.
    // Use this frame's actual render projection, before any command-buffer transitions.
    if (!VR::InFrame::Take(rec.id, list) ||
        VR::InFrame::CullInvisible(list, rec.proj_vfov, aspect, eye_half_width,
                                  horizontal_extent, vertical_extent) == 0) {
        static bool warmed = false;
        if (!warmed) {
            warmed = true; // the pipeline exists before the first item needs it
            vr_inframe->Warm(srgb, depth_format);
        }
        return false;
    }
    scheduler.EndRendering();
    GpuPassProfiler::MarkAt(scheduler, 0x1F4A'0000'0001ull, GpuPassProfiler::Group::VrHost,
                            "VR in-frame items (scene probe, depth copy, item draws)");
    depth->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    // The scene's light around our items (a box-filtered copy of X), then X as the target.
    x->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    vr_inframe->ProbeScene(scheduler.Commands(), x->GetImage(), x_format, depth_extent);
    if (stereo) {
        vr_inframe_right->ProbeScene(scheduler.Commands(), x->GetImage(), x_format, depth_extent,
                                     1);
    }
    // S1 (FAST bit 51): the eyes are made from the reconstructed output-size twin of X, so our
    // items go into it, against the scene depth blitted to its size (nearest, depth aspect).
    // Only when the UI layer (mode 1, VrUiArm) takes the eyes from that twin: otherwise the eyes
    // are X itself and the items must stay in X.
    auto* whole = vr_temporal && vr_temporal->s1.valid && stereo && vr_ui_layer &&
                          VR::UiLayerFrameMode() == 1 && color->id == vr_temporal->s1.x_id &&
                          color->uid == vr_temporal->s1.x_uid
                      ? &vr_temporal->s1
                      : nullptr;
    if (whole && !(vr_temporal->S1Depth(*depth) && vr_temporal->S1SrgbViews(srgb))) {
        whole = nullptr;
    }
    if (whole) {
        const auto cmd = scheduler.Commands();
        const bool stencil = depth_format == vk::Format::eD32SfloatS8Uint ||
                             depth_format == vk::Format::eD24UnormS8Uint ||
                             depth_format == vk::Format::eD16UnormS8Uint;
        const vk::ImageSubresourceRange both{
            stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                    : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth},
            0, 1, 0, 2};
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = whole->depth.image,
            .subresourceRange = both,
        };
        cmd.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 2},
            .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<s32>(depth_extent.width),
                             static_cast<s32>(depth_extent.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 2},
            .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<s32>(whole->width), static_cast<s32>(whole->height), 1}},
        };
        cmd.blitImage(depth->GetImage(), vk::ImageLayout::eTransferSrcOptimal, whole->depth.image,
                      vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        vr_temporal->S1ColorTo(vk::ImageLayout::eColorAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                               vk::AccessFlagBits2::eColorAttachmentRead |
                                   vk::AccessFlagBits2::eColorAttachmentWrite);
    } else {
        x->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                   vk::AccessFlagBits2::eColorAttachmentRead |
                       vk::AccessFlagBits2::eColorAttachmentWrite,
                   {});
    }
    VideoCore::ImageViewInfo view_info{};
    view_info.format = srgb;
    const auto& color_views = x->FindView(view_info, false);
    const vk::ImageView color_view = *color_views.image_view;
    VrInFrame::Target target{};
    target.color_view = color_view;
    target.color_format = srgb;
    target.swapped = swapped;
    target.extent = depth_extent;
    target.depth_image = depth->GetImage();
    target.depth_format = depth_format;
    target.depth_extent = depth_extent;
    target.zmin = best->zmin;
    target.zmax = best->zmax;
    if (whole) {
        target.extent = target.depth_extent = {whole->width, whole->height};
        target.depth_image = whole->depth.image;
    }
    VrInFrame::Camera camera{};
    camera.vfov = rec.proj_vfov;
    camera.aspect = aspect;
    camera.near_z = rec.near_z;
    camera.far_z = rec.far_z;
    bool drawn = true;
    for (u32 eye = 0; eye < (stereo ? 2u : 1u); ++eye) {
        target.color_view = whole ? *whole->srgb[eye] : eye ? *color_views.native_right : color_view;
        target.depth_layer = eye;
        camera.eye_x = (eye ? 1.0f : -1.0f) * eye_half_width;
        camera.collect_probe = eye == 0;
        const auto& projection = projections[eye];
        // The scene projection is remapped before its Vulkan Y flip. Our shader's position
        // already has that flip, so the Y offset has the opposite sign.
        camera.clip = {projection.x_scale, projection.y_scale,
                       projection.x_offset, -projection.y_offset};
        auto& renderer = eye ? vr_inframe_right : vr_inframe;
        const bool eye_drawn = renderer->Record(scheduler.Commands(), target, camera, list);
        drawn = drawn && eye_drawn;
    }
    // Our pipeline replaced the game's dynamic state: the next game draw sets all of it again.
    scheduler.GetDynamicState().Invalidate();
    vr_dibr.inframe = drawn;
    if (drawn) {
        VR::InFrame::NoteDrawn(); // the quick-item arc can be shown (Y opens it)
        if (whole) {
            ++whole->inframe;
        }
    }
    return drawn;
}

VideoCore::Image* Rasterizer::VrCompositeTarget() {
    if (!vr_dibr.color_id) {
        return nullptr;
    }
    return texture_cache.TryGetImage(vr_dibr.color_id, vr_dibr.color_uid);
}

VideoCore::Image* Rasterizer::VrUiTarget() {
    if (!vr_ui.armed) {
        return nullptr;
    }
    return texture_cache.TryGetImage(vr_ui.x_id, vr_ui.x_uid);
}

// The composite of a UI-layer frame. X, the target the UI is drawn into, is the DIBR capture's
// (whose scene copy the eyes are warped from), else the newest colour target of the frame's aspect
// before the composite, which is copied here for the eyes (AER, mono, a refused DIBR capture).
// Every refusal is decided here, before any UI draw: a refused frame keeps today's path entirely
// (the HUD panel in the eye images).
// FAST bit 59: the HUD at S1's output size (VrUiArm, VrUiShadowDraw). Needs bit 51 (S1) and
// ui_layer=1; without them nothing changes.
static constexpr auto S1HudOutput = static_cast<FastRender::Bit>(59);

void Rasterizer::VrUiArm(s32 mode) {
    vr_ui.mode = static_cast<u8>(mode);
    const auto refuse = [&](u32 bit, const char* why) {
        vr_ui.refused = why;
        UiWarnOnce(bit, fmt::format("{}; such frames keep the HUD in the eye images", why));
    };
    VideoCore::ImageId x_id{};
    u64 x_uid{};
    vk::ComponentMapping components{};
    if (vr_dibr.color_id) {
        x_id = vr_dibr.color_id;
        x_uid = vr_dibr.color_uid;
        components = vr_dibr.color_components;
        vr_ui.scene_in_warp = true;
    } else {
        const vk::Extent2D main{static_cast<u32>(std::lround(VR::MainAspect() * 1000.0f)), 1000};
        const VrColorTarget* color = nullptr;
        for (const auto& target : vr_dibr.color) {
            if (target.id && SameAspect(target.extent, main) &&
                (color == nullptr || target.last_draw > color->last_draw)) {
                color = &target;
            }
        }
        if (color == nullptr) {
            refuse(1u << 0, "no colour target of the frame's aspect before the composite");
            return;
        }
        x_id = color->id;
        x_uid = color->uid;
        components = color->components;
    }
    auto* x = texture_cache.TryGetImage(x_id, x_uid);
    if (x == nullptr) {
        refuse(1u << 1, "the composite target went away before the UI");
        return;
    }
    if (x->backing->num_samples != 1) {
        refuse(1u << 2, "the composite target is multisampled");
        return;
    }
    const vk::Extent2D extent{x->info.size.width, x->info.size.height};
    const vk::Format format = x->backing->image.image_ci.format;
    const auto& s = VR::GetSettings();
    // S1 (FAST bit 51): X's reconstructed output-size twin is this frame's eyes' scene.
    auto* whole = mode == 1 && !vr_ui.scene_in_warp && vr_temporal && vr_temporal->s1.valid &&
                          x->native_stereo && x_id == vr_temporal->s1.x_id &&
                          x_uid == vr_temporal->s1.x_uid &&
                          vr_temporal->s1.color_format == format
                      ? &vr_temporal->s1
                      : nullptr;
    // FAST bit 59 (S1HudOutput, default off): the HUD at the output size, as bbport's upscaler
    // redirect. K and W (the UI layer the headset shows) are made at S1's output size and every
    // shadowed UI draw is scaled into them (VrUiShadowDraw: viewports and scissors by output /
    // input, the depth/stencil from the output-size twin, VrTemporalCapture::S1UiDepth). X, the
    // game's own image (desktop mirror, fallback eyes), keeps the HUD at the input size.
    const bool hud_out = whole && FastRender::On(S1HudOutput) &&
                         (whole->width != extent.width || whole->height != extent.height);
    const vk::Extent2D layer_extent = hud_out ? vk::Extent2D{whole->width, whole->height} : extent;
    if (!vr_ui_layer->BeginFrame(format, layer_extent, components, s.ui_grey,
                                 x->native_stereo ? 2 : 1)) {
        UiWarnOnce(1u << 5,
                   fmt::format("the composite target's format is {}", vk::to_string(format)));
        refuse(1u << 3, "the composite target's format cannot be shadowed");
        return;
    }
    // The eyes' scene: the DIBR capture's copy, or a copy here (also the check's reference).
    if (!vr_ui.scene_in_warp) {
        scheduler.EndRendering();
        // S1 (FAST bit 51): the eyes' scene is X's reconstructed output-size twin (K and W, the
        // UI, stay at X's input size unless bit 58; the presenter takes the scene at its own
        // extent). Mode 1 only: the check (mode 2) compares X with this copy texel for texel.
        bool captured = false;
        if (whole) {
            vr_temporal->S1ColorTo(vk::ImageLayout::eTransferSrcOptimal,
                                   vk::PipelineStageFlagBits2::eTransfer,
                                   vk::AccessFlagBits2::eTransferRead);
            captured = vr_ui_layer->CaptureScene(scheduler.Commands(), whole->color.image,
                                                 vk::ImageLayout::eTransferSrcOptimal, format,
                                                 {whole->width, whole->height}, components, 2);
            whole->ui_scene += captured ? 1 : 0;
        } else {
            x->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       {});
            captured = vr_ui_layer->CaptureScene(scheduler.Commands(), x->GetImage(),
                                                 vk::ImageLayout::eTransferSrcOptimal, format,
                                                 extent, components, x->native_stereo ? 2 : 1);
        }
        if (!captured) {
            vr_ui_layer->EndFrame();
            refuse(1u << 4, "the scene could not be copied at the composite");
            return;
        }
    }
    vr_ui.armed = true;
    vr_ui.native = mode == 1;
    vr_ui.x_id = x_id;
    vr_ui.x_uid = x_uid;
    vr_ui.x_components = components;
    vr_ui.x_extent = layer_extent;
    vr_ui.x_format = format;
    vr_ui.hud_sx = hud_out ? static_cast<float>(layer_extent.width) / extent.width : 1.0f;
    vr_ui.hud_sy = hud_out ? static_cast<float>(layer_extent.height) / extent.height : 1.0f;
    vr_ui_ds_wrote = false;
    if (hud_out) {
        whole->ui_ds_dirty = true; // the game's depth/stencil is taken again at the first test
        ++whole->hud_frames;
        static bool hud_logged = false;
        if (!hud_logged) {
            hud_logged = true;
            LOG_INFO(Render_Vulkan,
                     "VR ui: S1 HUD at the output size (FAST bit 59): K and W {}x{} x2, the game's "
                     "UI of {}x{} scaled by {:.4f} x {:.4f}",
                     layer_extent.width, layer_extent.height, extent.width, extent.height,
                     vr_ui.hud_sx, vr_ui.hud_sy);
        }
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO(Render_Vulkan,
                 "VR ui: {} the UI of {}x{} {} (read as {}{}{}{}), eyes from the {} scene copy",
                 mode == 1 ? "layer on:" : "check only:", extent.width, extent.height,
                 vk::to_string(format), vk::to_string(components.r), vk::to_string(components.g),
                 vk::to_string(components.b), vk::to_string(components.a),
                 vr_ui.scene_in_warp ? "DIBR" : "UI layer's");
    }
}

// Every draw after the composite of an armed frame: true = draw it into K and W too (a colour
// draw into X). Counted for the `VR ui:` line.
bool Rasterizer::VrUiPrepare(const GraphicsPipeline* pipeline, const RenderState& state) {
    vr_ui_marker_draw = false;
    if (!vr_ui.armed || vr_ui.incomplete) {
        return false;
    }
    ++vr_ui.ui_draws;
    if (vr_ui.hud_sx != 1.0f || vr_ui.hud_sy != 1.0f) {
        // Bit 58: the previous UI draw may have written the game's depth/stencil (Scaleform's
        // masks): the output-size twin takes it again before the next draw that tests it.
        if (vr_ui_ds_wrote && vr_temporal) {
            vr_temporal->s1.ui_ds_dirty = true;
        }
        const auto& dc = std::as_const(ScratchDraw::Registers(liverpool)).depth_control;
        vr_ui_ds_wrote = state.depth_stencil_attachment.image_view &&
                         (dc.stencil_enable || (dc.depth_enable && dc.depth_write_enable) ||
                          state.depth_stencil_attachment.is_clear);
    }
    if (state.num_color_attachments == 0 || !cb_descs[0].first ||
        !state.color_attachments[0].image_view) {
        return false; // depth/stencil only (Scaleform's masks): drawn once, at native size
    }
    if (cb_descs[0].first != vr_ui.x_id) {
        ++vr_ui.offtarget;
        return false;
    }
    const auto& image = texture_cache.GetImage(cb_descs[0].first);
    if (image.image_uid != vr_ui.x_uid) {
        vr_ui.incomplete = true;
        UiWarnOnce(1u << 8, "the composite target was replaced during the UI");
        return false;
    }
    const auto& range = cb_descs[0].second.view_info.range;
    if (range.base.layer != 0 || range.base.level != 0) {
        vr_ui.incomplete = true;
        UiWarnOnce(1u << 9, "a UI draw targets another layer or mip of the composite target");
        return false;
    }
    // The large gameplay messages share two rectangle passes behind
    // their vector glyphs: a solid dim (VS 81d336ce / FS b7de74d9), then a
    // textured panel (VS 24042a9b / FS babda34d). Keep the original game draw,
    // but omit these rectangles from the VR UI replay while that banner plays.
    // The glyph draws, their colours/glow and ordinary menu rendering are untouched.
    if (vr_ui.native && VR::GetSettings().ui_banner_title &&
        VR::GetSettings().ui_death_distance_m > 0.0f &&
        VR::DrawnFrameUiClass() == VR::UiClass::None &&
        std::as_const(ScratchDraw::Registers(liverpool)).num_indices == 30) {
        const u64 vs = pipeline->GuestShaderKey(Shader::LogicalStage::Vertex);
        const u64 fs = pipeline->GuestShaderKey(Shader::LogicalStage::Fragment);
        const bool rectangle = (vs == 0x81d5812bbeb && fs == 0xb7e26dc85df) ||
                               (vs == 0x240c72546f9 && fs == 0xbac3ea580a0);
        if (rectangle) {
            const auto banner = VR::LatestBanner();
            const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (VR::Ui::BannerTextEffect(banner.type) && banner.serial != 0 &&
                now >= banner.ns && now - banner.ns < 7'000'000'000) {
                ++vr_ui.backdrop;
                return false;
            }
        }
    }
    if (attachment_feedback_loop) {
        // The draw reads X (a blur or a copy of the scene behind a panel): skipped, the live world
        // shows there instead of a flat copy of it (ui_backdrop=keep bakes it into the layer).
        ++vr_ui.backdrop;
        if (!VR::GetSettings().ui_backdrop_keep) {
            return false;
        }
    }
    const auto& key = pipeline->GetGraphicsKey();
    const auto& blend = key.blend_controls[0];
    if (!AffineBlend(blend, key.logic_op)) {
        ++vr_ui.nonaffine;
        UiWarnOnce(1u << 10,
                   fmt::format("a UI draw blends non-affinely (colour {}/{}/{}, logic op "
                               "{}); it is shadowed approximately",
                               static_cast<u32>(blend.color_src_factor),
                               static_cast<u32>(blend.color_dst_factor),
                               static_cast<u32>(blend.color_func), static_cast<u32>(key.logic_op)));
    }
    if (blend.enable &&
        (ReadsDstAlpha(blend.color_src_factor) || ReadsDstAlpha(blend.color_dst_factor))) {
        ++vr_ui.dst_alpha;
        UiWarnOnce(1u << 11, fmt::format("a UI draw blends with the destination alpha (colour "
                                         "{}/{}); K and W start with alpha 0",
                                         static_cast<u32>(blend.color_src_factor),
                                         static_cast<u32>(blend.color_dst_factor)));
    }
    // The game draws its HUD first and a menu over it: K and W are kept right before the first
    // draw the gameplay HUD never makes (the menu's dim, its panels), for the HUD shown under the
    // menu canvas (VrUiLayer::SnapshotHud, done in VrUiShadowDraw outside the render pass).
    const u64 sig = VrUiDrawSignature(pipeline);
    if (vr_ui_sigs.size() < 1024) {
        // A plain translucent rect: no stencil, the usual alpha blend, nothing sampled but X.
        bool solid =
            !std::as_const(ScratchDraw::Registers(liverpool)).depth_control.stencil_enable &&
            blend.enable && blend.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcAlpha &&
            blend.color_dst_factor == AmdGpu::BlendControl::BlendFactor::OneMinusSrcAlpha;
        for (const auto id : bound_images) {
            solid = solid && id == vr_ui.x_id;
        }
        vr_ui_sigs.push_back(sig);
        vr_ui_solid.push_back(solid ? 1 : 0);
    }
    if (vr_ui.hud_draws < 0) {
        const auto it = vr_hud_draws.find(sig);
        if (it == vr_hud_draws.end() || !it->second.hud) {
            vr_ui.hud_draws = static_cast<s32>(vr_ui.shadowed);
            vr_ui.first_other = sig;
            // Kept only for a full menu's frame (the resolve's menu HUD reads it).
            vr_ui_hud_snap = vr_ui.native && VR::GetSettings().ui_menu_hud &&
                             VR::DrawnFrameUiClass() == VR::UiClass::Full;
        }
    }
    vr_ui_marker_draw = VrUiIsLockMarker(state);
    return true;
}

bool Rasterizer::VrUiIsLockMarker(const RenderState& state) {
    if (!vr_ui.native || state.color_attachments[0].is_clear) {
        return false;
    }
    // bound_images: the colour targets and the depth / stencil (PrepareRenderState), then the
    // textures (BindResources): the marker's draw samples exactly one image.
    VideoCore::ImageId texture{};
    u32 textures = 0;
    for (const auto id : bound_images) {
        if (id && id != cb_descs[0].first && id != db_desc.first && id != texture) {
            texture = id;
            ++textures;
        }
    }
    if (textures != 1) {
        return false;
    }
    const auto& s = VR::GetSettings();
    const VR::Ui::LockMarker::Factors factors = VR::Ui::LockMarker::Clamp(
        VR::Tune::Get("ui.lock_marker_scale", s.ui_lock_marker_scale),
        VR::Tune::Get("ui.lock_marker_alpha", s.ui_lock_marker_alpha));
    if (!VR::Ui::LockMarker::Active(factors)) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (VR::IsLockedOn()) {
        g_marker_locked_at = now;
    } else if (now - g_marker_locked_at > std::chrono::seconds(1)) {
        return false;
    }
    const auto& image = texture_cache.GetImage(texture);
    const VAddr address = image.info.guest_address;
    if (image.info.size.width != LockMarkerAtlasSize ||
        image.info.size.height != LockMarkerAtlasSize || address < 0x10000 ||
        image.info.guest_size < 64 * 64) {
        return false;
    }
    const u64 head = AtlasHead(address, image.info.guest_size);
    auto& entry = g_marker_atlases[address];
    if (entry.hash == 0 || entry.head != head) {
        entry.head = head;
        entry.hash = AtlasHash(address, image.info.guest_size);
        if (g_marker_atlas_logs != 0) {
            --g_marker_atlas_logs;
            LOG_INFO(Render_Vulkan,
                     "VR ui: lock-on: a {}x{} atlas at {:#x} ({} bytes) sampled by a UI draw, "
                     "content {:#018x}: {}",
                     image.info.size.width, image.info.size.height, address,
                     image.info.guest_size, entry.hash,
                     entry.hash == LockMarkerAtlasHash ? "the lock-on marker's (scaled, dimmed)"
                                                       : "not the marker's");
        }
        if (g_marker_atlases.size() > 64) {
            std::erase_if(g_marker_atlases,
                          [address](const auto& e) { return e.first != address; });
        }
    }
    // Tests (VR_TUNE_FILE): ui.lock_marker_atlas_hi / _lo (the address >> 24 and its low 24 bits)
    // name an atlas to take as the marker's.
    const float test_hi = VR::Tune::Get("ui.lock_marker_atlas_hi", -1.0f);
    const float test_lo = VR::Tune::Get("ui.lock_marker_atlas_lo", -1.0f);
    const bool test_atlas = test_hi >= 0.0f && test_lo >= 0.0f &&
                            address == (static_cast<VAddr>(test_hi) << 24 |
                                        static_cast<VAddr>(test_lo));
    if (entry.hash != LockMarkerAtlasHash && !test_atlas) {
        return false;
    }
    ++vr_ui.marker_draws;
    vr_ui.marker_scale = factors.scale;
    vr_ui.marker_alpha = factors.alpha;
    return true;
}

// What tells one UI draw from another across frames: its shaders, blend, write mask, stencil
// test and sampled images (not its vertex count: a number's text changes it).
u64 Rasterizer::VrUiDrawSignature(const GraphicsPipeline* pipeline) {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    u64 h = 0xCBF29CE484222325ull;
    const auto mix = [&h](u64 v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0x100000001B3ull;
    };
    mix(key.stage_hashes[u32(Shader::LogicalStage::Vertex)]);
    mix(key.stage_hashes[u32(Shader::LogicalStage::Fragment)]);
    mix(RawWord(key.blend_controls[0]));
    mix(static_cast<u32>(key.write_masks[0]));
    const auto& dc = regs.depth_control;
    if (dc.stencil_enable) {
        mix(0x51ull | static_cast<u64>(dc.stencil_ref_func) << 8);
        mix(RawWord(regs.stencil_control));
    }
    for (const auto id : bound_images) {
        mix(texture_cache.GetImage(id).info.guest_address);
    }
    return h;
}

void Rasterizer::VrUiCommitHud(bool gameplay, bool plain) {
    if (!vr_ui.armed || vr_ui.incomplete || !gameplay) {
        return;
    }
    ++vr_hud_frames;
    for (size_t i = 0; i < vr_ui_sigs.size(); ++i) {
        auto& draw = vr_hud_draws[vr_ui_sigs[i]];
        if (draw.last == vr_hud_frames) {
            continue; // the same draw again in this frame
        }
        const bool in_row = draw.last + 1 == vr_hud_frames;
        draw.streak = in_row ? draw.streak + 1 : 1;
        draw.plain_streak = plain ? (in_row ? draw.plain_streak : 0) + 1 : 0;
        draw.last = vr_hud_frames;
        // A plain translucent rect counts only from frames with the HUD alone: a prompt's or a
        // subtitle's box is drawn just like the dim a menu starts with.
        const u32 streak = i < vr_ui_solid.size() && vr_ui_solid[i] ? draw.plain_streak
                                                                    : draw.streak;
        if (streak >= VrHudLearnFrames) {
            draw.hud = true;
        }
    }
    if (vr_hud_frames % 600 == 0 || vr_hud_draws.size() > VrHudMaxDraws) {
        std::erase_if(vr_hud_draws, [&](const auto& entry) {
            return entry.second.last + (entry.second.hud ? VrHudForgetFrames : 4) < vr_hud_frames;
        });
    }
}

namespace {

// FAST_GPU_PASS_MERGE and its _VERIFY (vk_pass_merge.h), read once at start-up.
const bool g_pass_merge = PassMerge::SwitchOn(std::getenv("FAST_GPU_PASS_MERGE"));
const bool g_pass_merge_verify =
    g_pass_merge && PassMerge::VerifyOn(std::getenv("FAST_GPU_PASS_MERGE_VERIFY"));

// Any clear the state's pass would begin with (colour, depth, stencil).
bool StateClears(const RenderState& state) {
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        if (state.color_attachments[i].is_clear != 0) {
            return true;
        }
    }
    return state.depth_stencil_attachment.depth_clear ||
           state.depth_stencil_attachment.stencil_clear;
}

// The draw's depth/stencil use as the committed dynamic state records it.
PassMerge::DepthStencilUse DepthStencilOfDynamic(const DynamicState& d, const RenderState& state) {
    const auto keeps = [](const StencilOps& ops) {
        return ops.fail_op == vk::StencilOp::eKeep && ops.pass_op == vk::StencilOp::eKeep &&
               ops.depth_fail_op == vk::StencilOp::eKeep;
    };
    return {
        .has_depth = state.depth_stencil_attachment.has_depth,
        .has_stencil = state.depth_stencil_attachment.has_stencil,
        .depth_test = d.depth_test_enabled,
        .depth_write = d.depth_write_enabled,
        .stencil_test = d.stencil_test_enabled,
        .front = {d.stencil_front_write_mask, keeps(d.stencil_front_ops)},
        .back = {d.stencil_back_write_mask, keeps(d.stencil_back_ops)},
    };
}

// ... and as the guest registers give it (UpdateDepthStencilState's rules, derived again).
PassMerge::DepthStencilUse DepthStencilOfRegisters(const AmdGpu::Regs& regs,
                                                   const RenderState& state) {
    using F = AmdGpu::StencilFunc;
    const auto keeps = [](F fail, F zpass, F zfail) {
        return fail == F::Keep && zpass == F::Keep && zfail == F::Keep;
    };
    const auto& dc = regs.depth_control;
    const auto& sc = regs.stencil_control;
    const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
    const PassMerge::StencilFace front{
        stencil_clear ? 0u : regs.stencil_ref_front.stencil_write_mask,
        keeps(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front)};
    const PassMerge::StencilFace back =
        dc.backface_enable
            ? PassMerge::StencilFace{stencil_clear ? 0u : regs.stencil_ref_back.stencil_write_mask,
                                     keeps(sc.stencil_fail_back, sc.stencil_zpass_back,
                                           sc.stencil_zfail_back)}
            : front;
    return {
        .has_depth = state.depth_stencil_attachment.has_depth,
        .has_stencil = state.depth_stencil_attachment.has_stencil,
        .depth_test = dc.depth_enable && regs.depth_buffer.DepthValid(),
        .depth_write = dc.depth_write_enable && !regs.depth_render_control.depth_clear_enable,
        .stencil_test = dc.stencil_enable && regs.depth_buffer.StencilValid(),
        .front = front,
        .back = back,
    };
}

} // Anonymous namespace

// FAST_GPU_PASS_MERGE (vk_pass_merge.h): a UI draw whose X pass is still open goes into X first
// and into its shadows after, so X's pass goes on, when nothing the shadows read can differ: the
// depth/stencil they test (X's draw writes none), its own targets as textures, memory the shaders
// write, clears, and the draws whose shadows other code hooks keep the old order.
bool Rasterizer::VrUiShadowAfter(const GraphicsPipeline* pipeline, const RenderState& state) {
    auto& m = vr_ui_merge;
    if ((++m.draws & 8191) == 0) {
        ReportPassMerge();
    }
    PassMerge::Inputs in{.on = g_pass_merge};
    if (in.on) {
        const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
        in.pass_open = scheduler.Continues(state);
        // Hooks that record into this draw (the lock-on marker's pair, the HUD snapshot, the
        // foreground probe, the hunter's split draws, an AA replacement, object motion): as before.
        in.hook = vr_ui_marker_draw || vr_ui_hud_snap || probe_selected ||
                  recording_vr.split != nullptr || recording_vr.body_split != nullptr ||
                  recording_vr.hands_split != nullptr || (vr_warp && vr_aa_action != 0) ||
                  pipeline->HasVrMotion();
        in.scaled = vr_ui.hud_sx != 1.0f || vr_ui.hud_sy != 1.0f;
        in.clears = StateClears(state);
        // A target bound as a texture too: the shadows would sample this draw's output.
        in.feedback = attachment_feedback_loop;
        for (u32 cb = 0; cb < state.num_color_attachments && !in.feedback; ++cb) {
            const auto id = cb_descs[cb].first;
            in.feedback = id && texture_cache.GetImage(id).binding.is_bound;
        }
        if (db_desc.first && !in.feedback) {
            in.feedback = texture_cache.GetImage(db_desc.first).binding.is_bound;
        }
        in.writes = regs.vgt_strmout_config.raw != 0;
        for (const Shader::Info* stage : pipeline->GetStages()) {
            in.writes = in.writes || (stage != nullptr && PipelineSkip::StageMayWrite(*stage));
        }
        in.depth_stencil = PassMerge::WritesDepthStencil(
            DepthStencilOfDynamic(scheduler.GetDynamicState(), state));
    }
    PassMerge::Fallback verdict = PassMerge::Decide(in);
    if (verdict == PassMerge::NumFallbacks && g_pass_merge_verify) [[unlikely]] {
        // The same decision a second way: the depth/stencil writes from the guest registers, a
        // target bound as a texture from the bound image list (targets first, then the textures:
        // a target there twice is sampled too).
        ++m.verified;
        const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
        const bool depth_stencil =
            PassMerge::WritesDepthStencil(DepthStencilOfRegisters(regs, state));
        bool sampled = false;
        const auto twice = [&](VideoCore::ImageId id) {
            return id && std::count(bound_images.begin(), bound_images.end(), id) > 1;
        };
        for (u32 cb = 0; cb < state.num_color_attachments && !sampled; ++cb) {
            sampled = twice(cb_descs[cb].first);
        }
        sampled = sampled || twice(db_desc.first);
        if (depth_stencil || sampled) {
            ++m.mismatches;
            if (m.mismatches <= 32 || (m.mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST pass_merge verify: mismatch #{}: the registers say depth/stencil "
                          "writes {}, the bound images say a target sampled {} (vs {:#x} fs "
                          "{:#x}); the draw keeps the old order",
                          m.mismatches, depth_stencil ? 1 : 0, sampled ? 1 : 0,
                          VertexHash(pipeline),
                          pipeline->GuestShaderKey(Shader::LogicalStage::Fragment));
            }
            verdict = PassMerge::Disagree;
        }
    }
    if (verdict != PassMerge::NumFallbacks) {
        ++m.fallback[verdict];
        return false;
    }
    ++m.after;
    return true;
}

void Rasterizer::ReportPassMerge() {
    if constexpr (Common::Log::Enabled) { // BBVR_NO_LOGS: no line to build
        const auto& m = vr_ui_merge;
        std::string reasons;
        for (u32 r = 0; r < PassMerge::NumFallbacks; ++r) {
            reasons +=
                fmt::format("{}{}={}", r ? " " : "", PassMerge::FallbackNames[r], m.fallback[r]);
        }
        LOG_INFO(Render_Vulkan,
                 "FAST pass_merge: on={} verify={} ui_draws={} drawn_first={} shadow_passes={} "
                 "kept_open={} continued={} clear_ends={} memo_resumed={} old_order [{}] "
                 "verified={} mismatches={}",
                 g_pass_merge ? 1 : 0, g_pass_merge_verify ? 1 : 0, m.draws, m.after, m.shadows,
                 m.kept, m.continued, m.clear_ends, m.memo_resumed, reasons, m.verified,
                 m.mismatches);
    }
}

// The draw into W, then into K: the game's state with colour attachment 0 swapped for the target
// (MRT attachments 1.. dropped), the depth and stencil writes masked (the game's own pass after
// these writes them once; the shadows test the same masks). Push constants, descriptors and the
// other dynamic state carry over between render passes. `after_draw` (FAST_GPU_PASS_MERGE): the
// game's draw into X came first; then into K, then into W (PassMerge::Order).
void Rasterizer::VrUiShadowDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                                VrDrawCall issue, bool after_draw) {
    // ui_menu_hud_off_canvas: the HUD rows of K and W, reset after they were kept (below).
    std::optional<vk::Rect2D> hud_clear;
    const bool marker = vr_ui_marker_draw;
    vr_ui_marker_draw = false;
    if (marker) {
        vr_ui.marker_after = 0;
    } else if (vr_ui.marker_draws != 0) {
        ++vr_ui.marker_after; // UI over the marker: the resolve puts the marker under the UI
    }
    if (vr_ui_hud_snap && !marker) { // the marker never touches K and W: the next draw keeps them
        // The first draw that is not the HUD's: K and W so far are the HUD alone.
        vr_ui_hud_snap = false;
        scheduler.EndRendering();
        const vk::Extent2D x = vr_ui.x_extent;
        vk::Rect2D rows{{0, 0}, x};
        if (static_cast<float>(x.width) / static_cast<float>(x.height) < 16.0f / 9.0f - 0.02f) {
            const u32 h = static_cast<u32>(std::lround(x.width * 9.0f / 16.0f));
            rows = {{0, static_cast<s32>((x.height - h) / 2)}, {x.width, h}};
        }
        // The band's top, where the status clusters are (stage y 0 .. 260 of 1080).
        rows.extent.height = std::min(rows.extent.height, rows.extent.height * 260 / 1080 + 16);
        vr_ui_layer->SnapshotHud(scheduler.CommandBuffer(), rows);
        // The HUD is now kept for under the menu canvas (the resolve's menu HUD): taken out of K
        // and W, the menu draws from here on onto its rows without it, so the canvas never shows
        // the HUD a large menu's crop reaches (the quick item's name beside the travel list and
        // the Controls page, mock 2026-10-07). Exact: K and W are only ever blended onto.
        if (VR::GetSettings().ui_menu_hud_off_canvas &&
            vr_ui_layer->HudSnapState() == VrUiLayer::HudSnap::Kept) {
            hud_clear = rows;
        }
    }
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetDepthWriteEnabled(false);
    dynamic_state.SetStencilWriteMasks(0, 0);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(false);
    // FAST bit 59: K and W at S1's output size; the game's viewports and scissors (input size)
    // scaled into them, restored for the game's own draw below.
    const bool hud_scaled = vr_ui.hud_sx != 1.0f || vr_ui.hud_sy != 1.0f;
    std::optional<std::pair<Viewports, Scissors>> game_vp;
    if (hud_scaled) {
        game_vp.emplace(dynamic_state.viewports, dynamic_state.scissors);
        Viewports viewports = dynamic_state.viewports;
        Scissors scissors = dynamic_state.scissors;
        const s32 w = static_cast<s32>(vr_ui.x_extent.width);
        const s32 h = static_cast<s32>(vr_ui.x_extent.height);
        for (auto& v : viewports) {
            v.x *= vr_ui.hud_sx;
            v.y *= vr_ui.hud_sy;
            v.width *= vr_ui.hud_sx;
            v.height *= vr_ui.hud_sy;
        }
        for (auto& r : scissors) {
            const s32 x0 = std::clamp(
                static_cast<s32>(std::floor(r.offset.x * vr_ui.hud_sx)), 0, w);
            const s32 y0 = std::clamp(
                static_cast<s32>(std::floor(r.offset.y * vr_ui.hud_sy)), 0, h);
            const s32 x1 = std::clamp(static_cast<s32>(std::ceil(
                (static_cast<float>(r.offset.x) + r.extent.width) * vr_ui.hud_sx)), x0, w);
            const s32 y1 = std::clamp(static_cast<s32>(std::ceil(
                (static_cast<float>(r.offset.y) + r.extent.height) * vr_ui.hud_sy)), y0, h);
            r = vk::Rect2D{{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
        }
        dynamic_state.SetViewports(viewports);
        dynamic_state.SetScissors(scissors);
    }
    dynamic_state.Commit(instance, scheduler.Commands());
    const vk::Format view_format = instance.GetSupportedFormat(
        cb_descs[0].second.view_info.format, vk::FormatFeatureFlagBits2::eColorAttachment);
    bool drawn = false;
    // The pair in PassMerge::Order: W (Grey), then K (Black) before the game's draw, as always;
    // K, then W after it (`after_draw`).
    std::array<VrUiLayer::Which, 2> pair{};
    {
        u32 n = 0;
        for (const auto target : PassMerge::Order(after_draw)) {
            if (target != PassMerge::Target::X) {
                const bool w = target == PassMerge::Target::W;
                pair[n++] = marker ? (w ? VrUiLayer::MarkGrey : VrUiLayer::MarkBlack)
                                   : (w ? VrUiLayer::Grey : VrUiLayer::Black);
            }
        }
    }
    auto& merge = vr_ui_merge;
    for (const auto which : pair) {
        // FAST_GPU_PASS_MERGE (vk_pass_merge.h): no explicit end while TargetView records nothing
        // for the target; BeginRendering below ends the open pass when the shadow's state
        // differs, and goes on with it when it is this target's pass already.
        const bool keep = PassMerge::KeepOpen(g_pass_merge, scheduler.IsRendering(),
                                              vr_ui_layer->TargetBound(which), hud_scaled);
        if (!keep) {
            scheduler.EndRendering();
        }
        const bool check = keep && g_pass_merge_verify && scheduler.RecorderEncoding();
        const u64 packets = check ? scheduler.RecorderCommandPackets() : 0;
        RenderState shadow = state;
        auto& attachment = shadow.color_attachments[0];
        attachment.image_view =
            vr_ui_layer->TargetView(scheduler.Commands(), which, view_format);
        if (check && scheduler.RecorderCommandPackets() != packets) [[unlikely]] {
            // A command recorded with a pass open (TargetView's layout barrier): never expected
            // for a target TargetBound called bound.
            ++merge.mismatches;
            if (merge.mismatches <= 32 || (merge.mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST pass_merge verify: mismatch #{}: TargetView recorded a command "
                          "inside an open pass (target {})",
                          merge.mismatches, static_cast<u32>(which));
            }
        }
        if (!attachment.image_view) {
            vr_ui.incomplete = true;
            break;
        }
        attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        if (attachment.is_clear) {
            vr_ui_layer->NoteGameClear(which); // X is cleared in this pass: K and W too
        } else {
            std::array<u32, 4> bits{};
            if (vr_ui_layer->TakeClear(which, bits)) {
                attachment.is_clear = 1;
                attachment.clear_value = bits;
            }
        }
        for (u32 i = 1; i < shadow.num_color_attachments; ++i) {
            shadow.color_attachments[i].image_view = VK_NULL_HANDLE;
        }
        if (hud_scaled) {
            shadow.width = static_cast<u16>(vr_ui.x_extent.width);
            shadow.height = static_cast<u16>(vr_ui.x_extent.height);
            if (shadow.depth_stencil_attachment.image_view) {
                // The game's depth/stencil is input-size: its output-size twin instead, taken
                // again from it (outside the pass) when a draw tests it after the game wrote it.
                const auto& dc = std::as_const(ScratchDraw::Registers(liverpool)).depth_control;
                auto* ds = db_desc.first ? &texture_cache.GetImage(db_desc.first) : nullptr;
                const vk::ImageView twin =
                    ds && vr_temporal ? vr_temporal->S1UiDepth(
                                            *ds, dc.stencil_enable || dc.depth_enable)
                                      : vk::ImageView{};
                if (!twin) {
                    vr_ui.incomplete = true;
                    UiWarnOnce(1u << 13, "bit 58: the HUD's depth/stencil has no output-size "
                                         "twin; that frame's layer is incomplete");
                    break;
                }
                shadow.depth_stencil_attachment.image_view = twin;
                shadow.depth_stencil_attachment.image_layout =
                    vk::ImageLayout::eDepthStencilAttachmentOptimal;
            }
            if (vr_temporal) {
                ++vr_temporal->s1.hud_draws;
            }
        }
        ++merge.shadows;
        if (keep) {
            if (StateClears(shadow)) {
                // A clear asked again would not run in a pass that goes on: ended as before.
                ++merge.clear_ends;
                scheduler.EndRendering();
            } else {
                ++merge.kept;
                merge.continued += scheduler.Continues(shadow) ? 1 : 0;
            }
        }
        GpuPassProfiler::LabelPassAt(scheduler, shadow, 0x0151'0000'0001ull,
                                     GpuPassProfiler::Group::VrHost,
                                     "VR UI layer shadow draws (K/W)");
        scheduler.BeginRendering(shadow);
        const auto cmdbuf = scheduler.Commands();
        if (hud_clear) {
            // Inside the pass: the rows back to the target's clear colour (the render area is
            // the attachment's size from the origin).
            const s32 x0 = std::clamp(hud_clear->offset.x, 0, static_cast<s32>(shadow.width));
            const s32 y0 = std::clamp(hud_clear->offset.y, 0, static_cast<s32>(shadow.height));
            const s32 x1 = std::clamp(hud_clear->offset.x +
                                          static_cast<s32>(hud_clear->extent.width),
                                      x0, static_cast<s32>(shadow.width));
            const s32 y1 = std::clamp(hud_clear->offset.y +
                                          static_cast<s32>(hud_clear->extent.height),
                                      y0, static_cast<s32>(shadow.height));
            if (x1 > x0 && y1 > y0) {
                vk::ClearAttachment clear{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .colorAttachment = 0,
                };
                clear.clearValue.color = vr_ui_layer->ClearColor(which);
                const vk::ClearRect rect{
                    .rect = vk::Rect2D{vk::Offset2D{x0, y0},
                                       vk::Extent2D{static_cast<u32>(x1 - x0),
                                                    static_cast<u32>(y1 - y0)}},
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };
                cmdbuf.clearAttachments(clear, rect);
                static u32 clears = 0;
                if (which == VrUiLayer::Black && clears++ % 1200 == 0) {
                    LOG_INFO(Render_Vulkan,
                             "VR ui: menu HUD off the canvas (ui_menu_hud_off_canvas): rows {}..{} "
                             "of K and W reset after they were kept ({} menu frames so far)",
                             y0, y1, clears);
                }
            }
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        issue(cmdbuf);
        drawn = true;
    }
    if (game_vp) {
        dynamic_state.SetViewports(game_vp->first);
        dynamic_state.SetScissors(game_vp->second);
    }
    // The game's own writes for its pass.
    UpdateDepthStencilState();
    UpdateColorBlendingState(pipeline);
    dynamic_state.Commit(instance, scheduler.Commands());
    if (drawn) {
        ++vr_ui.shadowed;
    }
    // FAST_GPU_PASS_MERGE: X's pass `state`, the memo's, ended only for these shadow passes
    // (VrUiMemoResumes). The memo changes only in BeginRendering, the next draw's check.
    vr_ui_resume.valid = after_draw && scheduler.IsRendering() && BeginMemoHolds(state);
    vr_ui_resume.passes = scheduler.PassBegins();
}

// FAST_GPU_PASS_MERGE (vk_pass_merge.h): the begin-state memo (bit 2) holds while X's pass is open
// (nothing ended it, so none of its targets changed layout). After a swapped UI draw the open pass
// is its shadow pair's instead: since X's pass ended only K's and W's passes were recorded, which
// touch none of X's targets (no transition; the depth/stencil an attachment in its layout, tested
// only), and nothing has begun or ended a pass since (a barrier, upload or dispatch would end it).
// The memo is then as valid as with X's pass still open; the hit's own checks (layouts, dirty
// images, meta clears, the registry epoch, the tick) run as always.
bool Rasterizer::VrUiMemoResumes() const {
    return vr_ui_resume.valid && scheduler.IsRendering() &&
           scheduler.PassBegins() == vr_ui_resume.passes;
}

std::optional<RenderState> Rasterizer::VrUiMemoVerify(const GraphicsPipeline* pipeline,
                                                     const RenderState& state) {
    auto& m = vr_ui_merge;
    ++m.memo_resumed;
    if (!g_pass_merge_verify) {
        return std::nullopt;
    }
    // FAST_GPU_PASS_MERGE_VERIFY: the full path as on a miss (VR_PERF_CHECK's comparison).
    ++m.verified;
    const RenderState reference = BeginRenderingFull(pipeline);
    if (reference == state && !attachment_feedback_loop) {
        return std::nullopt;
    }
    ++m.mismatches;
    if (m.mismatches <= 32 || (m.mismatches & 1023) == 0) {
        LOG_ERROR(Render_Vulkan,
                  "FAST pass_merge verify: mismatch #{}: the begin-state memo resumed after the "
                  "shadow passes differs from the full render state (feedback {}); the full one "
                  "is used",
                  m.mismatches, attachment_feedback_loop ? 1 : 0);
    }
    return reference;
}

// A dispatch in the UI phase that binds X (a compute pass writing it): the K/W pair misses it.
void Rasterizer::VrUiNoteDispatch() {
    for (const auto id : bound_images) {
        if (id == vr_ui.x_id) {
            ++vr_ui.x_writes;
            vr_ui.incomplete = true;
            UiWarnOnce(1u << 12, "a compute pass uses the composite target during the UI; that "
                                 "frame's layer is incomplete");
            return;
        }
    }
}

std::string Rasterizer::VrUiLogTag() const {
    if (!vr_ui.armed) {
        return vr_ui.marker ? "after" : "-";
    }
    return fmt::format("{}/s{}/o{}/b{}", vr_ui.ui_draws, vr_ui.shadowed, vr_ui.offtarget,
                       vr_ui.backdrop);
}

void Rasterizer::VrEndFrame() {
    CompileTrace::EndFrame();
    if (vr_temporal_enabled) VrTemporalEndFrame();
    VrNativeAuditEnd();
    VrProbeEndFrame();
    vr_dibr = {};
    vr_ui = {};
    vr_ui_sigs.clear();
    vr_ui_solid.clear();
    vr_ui_hud_snap = false;
    VrDrawsEndFrame();
}

} // namespace Vulkan
