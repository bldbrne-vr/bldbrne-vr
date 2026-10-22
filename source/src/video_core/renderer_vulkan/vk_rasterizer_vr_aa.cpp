// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini aa_mode (core/vr/aa_mode.h; the VR page's Graphics > Anti-aliasing): the game's FXAA
// draw, decided per frame in immersive DIBR frames. Bloodborne ends its post chain with a copy of
// the scene colour A into B (fs 0xf7221902253) and FXAA 3.11 from B back into A (fs
// 0x30a78adffc8, vr.ini aa_fxaa_fs). aa_mode=off skips the FXAA draw (VrFilterDraw: A keeps the
// scene as rendered, the same pixels as the presets' "Disable AA" patch, which removes the pair);
// aa_mode=smaa lets the draw bind its images (the texture cache resolves B and A, transitions
// them, uploads what they need) and then records SMAA from B into A (renderer_vulkan/vr_smaa.h)
// instead of the draw; aa_mode=taa records the render TAA's resolve there instead
// (renderer_vulkan/vr_render_taa.h: B rendered with this frame's sub-pixel jitter, a history
// reprojected with the scene depth and the frame's camera record), and SMAA when it refuses.
// Nothing in game memory changes, so a switch applies at the next frame and anything refused (an
// image SMAA cannot take, a Vulkan failure) draws the FXAA as before.

#include <algorithm>
#include <chrono>
#include <string>

#include "common/logging/log.h"
#include "core/vr/aa_mode.h"
#include "core/vr/graphics_settings.h"
#include "video_core/native_stereo/native_stereo.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_render_taa.h"
#include "video_core/renderer_vulkan/vr_dibr_dlss.h"
#include "video_core/renderer_vulkan/vr_smaa.h"
#include "video_core/renderer_vulkan/vr_temporal_capture.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

// FAST bit 41 (SmaaRecorder): SMAA is recorded through the scheduler's RecorderProxy instead of a
// raw command buffer. scheduler.CommandBuffer() (the old path, bit off) drains the threaded
// recorder (stage B waits for the recorder thread to replay everything encoded so far) and
// borrows the raw handle, after which every later command of the command buffer is recorded
// inline on stage B until the next submit's Configure. Through the proxy, SMAA's commands
// (barriers, binds, push descriptors, push constants, dispatches: 30 per stereo frame, 21 with
// the per-eye intermediates below) are encoded like the draws around them and replayed by the
// recorder thread, in the same order, into the same command buffer: the GPU receives the same
// Vulkan commands in the same sequence (with bit 35 a pipeline or push-constant command that
// rebinds what is bound is left out), so the output cannot differ and this half of the bit needs
// no verify of its own. With the recorder off or a raw handle already borrowed, each proxy call
// is the same direct call the raw path makes. The bit also gives the second eye its own SMAA
// intermediates (VrSmaa::Frame::per_eye_targets: 3 barriers per stereo frame instead of 6,
// bit-exact, checked by FAST_SMAA_RECORDER_VERIFY=N). It is read once per frame, so it may change
// between frames. One "FAST smaa_recorder" line every 2 s.
static constexpr auto SmaaRecorder = static_cast<FastRender::Bit>(41);

namespace {

// Bit 41 counters. One writer: the thread recording the game's command stream (stage B, or the
// GPU command thread when serial; recording changes threads only through the DrawPipe hand-off,
// which orders these plain counters like the rest of the recording state).
struct SmaaRecorderCounters {
    u64 recorded{};     // SMAA recorded in place of the FXAA draw
    u64 refused{};      // Record refused (the FXAA drawn)
    u64 raw_avoided{};  // bit on, recorder encoding: no drain, the rest of the buffer threaded
    u64 direct{};       // bit on, recorder off or already raw: nothing to avoid
    u64 raw_borrowed{}; // bit off: the old raw borrow
};
SmaaRecorderCounters g_smaa_recorder{};

void NoteSmaaRecorder(bool fast, bool encoding, bool recorded, const VrSmaa& smaa) {
    auto& c = g_smaa_recorder;
    if (!fast) {
        ++c.raw_borrowed;
    } else if (encoding) {
        ++c.raw_avoided;
    } else {
        ++c.direct;
    }
    if (recorded) {
        ++c.recorded;
    } else {
        ++c.refused;
    }
    static auto last = std::chrono::steady_clock::now();
    static SmaaRecorderCounters seen{};
    static VrSmaa::Stats seen_smaa{};
    static u64 seen_frames = FastRender::g_counters.frames.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) {
        return;
    }
    last = now;
    const VrSmaa::Stats s = smaa.GetStats();
    const u64 frames_now = FastRender::g_counters.frames.load(std::memory_order_relaxed);
    const double frames = static_cast<double>(frames_now - seen_frames);
    const auto per_frame = [frames](u64 value, u64 then) {
        return frames > 0 ? static_cast<double>(value - then) / frames : 0.0;
    };
    const u64 records = c.recorded - seen.recorded;
    LOG_INFO(Render_Vulkan,
             "FAST smaa_recorder: frames={:.0f} smaa/f={:.3f} raw_avoided/f={:.3f} "
             "direct/f={:.3f} raw_borrowed/f={:.3f} refused/f={:.3f} per_eye/f={:.3f} "
             "barriers/smaa={:.2f} verify_recorded={} compared={} mismatches={}",
             frames, per_frame(c.recorded, seen.recorded),
             per_frame(c.raw_avoided, seen.raw_avoided), per_frame(c.direct, seen.direct),
             per_frame(c.raw_borrowed, seen.raw_borrowed), per_frame(c.refused, seen.refused),
             per_frame(s.per_eye_frames, seen_smaa.per_eye_frames),
             records ? static_cast<double>(s.barrier_calls - seen_smaa.barrier_calls) /
                           static_cast<double>(records)
                     : 0.0,
             s.verify_recorded, s.verify_compared, s.verify_mismatched);
    seen = c;
    seen_smaa = s;
    seen_frames = frames_now;
}

// VrAaNote outcomes (one log line whenever the outcome changes).
enum Outcome : u8 { Drawn = 0, Skipped = 1, Smaa = 2, Refused = 3, Taa = 4 };

const char* OutcomeText(u8 o) {
    switch (o) {
    case Drawn:
        return "the game's FXAA drawn";
    case Skipped:
        return "spatial anti-aliasing bypassed";
    case Smaa:
        return "SMAA 1x in place of the game's FXAA";
    case Taa:
        return "render TAA in place of the game's FXAA";
    default:
        return "SMAA refused, the game's FXAA drawn";
    }
}

} // Anonymous namespace

u8 Rasterizer::VrAaDecide(const GraphicsPipeline* pipeline, bool indirect) {
    const auto& s = VR::GetSettings();
    const u64 fs = pipeline->GuestShaderKey(Shader::LogicalStage::Fragment);
    if (indirect || fs != s.aa_fxaa_fs) {
        return static_cast<u8>(VR::Aa::Action::Draw);
    }
    // VR_TUNE_FILE aa_mode=0 / 1 / 2 / 3 (tests: fxaa / smaa / taa / off): the setting changed
    // while the game runs, queued like the VR page (VR::ApplySetting) for the next boundary.
    if (VR::Tune::Active()) {
        const float t = VR::Tune::Get("aa_mode", -1.0f);
        if (t >= 0.0f) {
            const auto want = static_cast<VR::Aa::Mode>(std::clamp(static_cast<int>(t), 0, 3));
            if (want != VR::GetRequestedAaMode()) {
                LOG_INFO(Render_Vulkan, "VR aa: aa_mode={} (VR_TUNE_FILE, while running)",
                         VR::Aa::Name(want));
                VR::ApplySetting("aa_mode", VR::Aa::Name(want));
            }
        }
    }
    if (VR::DibrFrameActive() && vr_ui_layer && VR::UiLayerFrameMode() == 1 &&
        vr_dibr_dlss && vr_dibr_dlss->Ready()) {
        VrAaNote(Skipped, "experimental DIBR DLSS center reconstruction; saved AA unchanged");
        return static_cast<u8>(VR::Aa::Action::Skip);
    }
    const VR::Aa::DrawFacts facts{
        .fs_hash = fs,
        .fxaa_fs = s.aa_fxaa_fs,
        .immersive = VR::DibrFrameActive() || (NativeStereo::Enabled() && VR::IsImmersive()),
        // Only bypass AA after this frame has actually been reconstructed and merged. S1 (FAST
        // bit 51): its provider runs after this draw, at the composite, on the frame without AA.
        .upscaler = vr_temporal && vr_temporal->frame == vr_temporal_frame &&
                    (vr_temporal->reconstructed || vr_temporal->s1.pending),
    };
    const VR::Aa::Action action = VR::Aa::Decide(VR::GetActiveAaMode(), facts);
    if (action == VR::Aa::Action::Draw) {
        VrAaNote(Drawn, facts.immersive ? std::string{"aa_mode=fxaa"}
                                        : std::string{"not an immersive frame"});
    } else if (action == VR::Aa::Action::Skip) {
        VrAaNote(Skipped, facts.upscaler ? "DLSS/FSR active; saved AA unchanged" : "aa_mode=off");
    } else if (action == VR::Aa::Action::Taa &&
               (s.taa_debug == 3 ||
                (VR::Tune::Active() && VR::Tune::Get("taa.debug", 0.0f) >= 3.0f))) {
        // taa_debug=3 (tests): the raw jittered frame, nothing resolved (shows the jitter).
        VR::NoteTaaResolve(0, 0, true);
        VrAaNote(Skipped, "aa_mode=taa, taa_debug=3: the raw jittered frame");
        return static_cast<u8>(VR::Aa::Action::Skip);
    }
    return static_cast<u8>(action);
}

void Rasterizer::VrAaNote(u8 outcome, const std::string& detail) {
    if (outcome < 5) {
        ++vr_aa_log.counts[outcome];
    }
    if (outcome == vr_aa_log.last) {
        return;
    }
    vr_aa_log.last = outcome;
    // Every change while it is rare (a switch on the VR page, a cinematic screen), then every
    // 50th (frames that keep flipping between immersive and not).
    if (++vr_aa_log.lines <= 24 || vr_aa_log.lines % 50 == 0) {
        LOG_BENCHMARK_INFO(Render_Vulkan,
                 "VR aa: {} ({}; FXAA draws so far: {} drawn, {} skipped, {} SMAA, {} TAA, {} "
                 "refused)",
                 OutcomeText(outcome), detail, vr_aa_log.counts[Drawn], vr_aa_log.counts[Skipped],
                 vr_aa_log.counts[Smaa], vr_aa_log.counts[Taa], vr_aa_log.counts[Refused]);
    }
}

bool Rasterizer::VrAaReplace(const GraphicsPipeline* pipeline, const RenderState& state) {
    const bool want_taa = vr_aa_action == static_cast<u8>(VR::Aa::Action::Taa);
    vr_aa_action = 0;
    // The draw's images as the texture cache bound them: its one texture B, its target A.
    VR::Aa::DrawImages di{};
    const auto& fs_stage = pipeline->GetStage(Shader::LogicalStage::Fragment);
    VideoCore::Image* b = nullptr;
    VideoCore::Image* a = nullptr;
    // bound_images: the colour targets and depth (PrepareRenderState), then the textures
    // (BindResources): B is the one image that is neither.
    const VideoCore::ImageId a_id = cb_descs[0].first;
    if (fs_stage.images.size() == 1) {
        VideoCore::ImageId b_id{};
        u32 others = 0;
        for (const auto id : bound_images) {
            if (id && id != a_id && id != db_desc.first && id != b_id) {
                b_id = id;
                ++others;
            }
        }
        if (others == 1) {
            b = &texture_cache.GetImage(b_id);
        } else if (others == 0 && a_id && bound_images.size() > 1) {
            b = &texture_cache.GetImage(a_id); // it samples its own target (refused below)
        }
    }
    if (a_id && state.num_color_attachments == 1) {
        a = &texture_cache.GetImage(a_id);
    }
    const auto bytes = [](const VideoCore::Image* image) -> u32 {
        return VrSmaa::ByteOrder(image->backing->image.image_ci.format) >= 0 ? 4u : 0u;
    };
    if (b != nullptr) {
        di.has_texture = true;
        di.texture_w = b->info.size.width;
        di.texture_h = b->info.size.height;
        di.texture_bytes_per_texel = bytes(b);
    }
    if (a != nullptr) {
        di.has_target = true;
        di.target_w = a->info.size.width;
        di.target_h = a->info.size.height;
        di.target_bytes_per_texel = bytes(a);
        di.target_samples = a->backing->num_samples;
    }
    di.same_image = a != nullptr && a == b;
    std::string why;
    if (!VR::Aa::Usable(di, &why)) {
        VrAaNote(Refused, why);
        return false;
    }
    const u32 layers = a->native_stereo && b->native_stereo ? 2u : 1u;
    if (a->native_stereo != b->native_stereo ||
        a->backing->image.image_ci.arrayLayers != layers ||
        b->backing->image.image_ci.arrayLayers != layers) {
        VrAaNote(Refused, "mismatched source/target eye layers");
        return false;
    }
    if (!vr_smaa) {
        static bool created = false;
        if (created) {
            VrAaNote(Refused, "SMAA unavailable");
            return false;
        }
        created = true;
        vr_smaa = std::make_unique<VrSmaa>(instance, scheduler);
    }
    if (!vr_smaa->Ready()) {
        VrAaNote(Refused, "SMAA unavailable");
        return false;
    }
    // The game writes its colour through the colour buffer's component swap (Bloodborne: red and
    // blue swapped, as the DIBR capture reads X back): SMAA's luma weights follow it.
    const vk::ComponentMapping c = LiverpoolToVK::ComponentMapping(
        pipeline->GetGraphicsKey().color_buffers[0].swizzle.Inverse());
    const bool swap_rb = c.r == vk::ComponentSwizzle::eB && c.b == vk::ComponentSwizzle::eR;
    // Out of the render pass; B readable and A writable by compute shaders (the texture cache's
    // own layout tracking: the next draw that reads A transitions it from here).
    scheduler.EndRendering();
    b->Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});
    a->Transit(vk::ImageLayout::eGeneral,
               vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead, {});
    // aa_mode=taa: the render TAA's resolve; a frame it refuses gets SMAA below.
    if (want_taa) {
        std::string taa_why;
        if (VrTaaReplace(*b, *a, &taa_why)) {
            return true;
        }
        VrTaaNoteRefusal(taa_why);
    }
    const bool fast_smaa = FastRender::On(SmaaRecorder);
    const VrSmaa::Frame frame{
        .src = b->GetImage(),
        .src_format = b->backing->image.image_ci.format,
        .src_uid = b->image_uid,
        .dst = a->GetImage(),
        .dst_format = a->backing->image.image_ci.format,
        .dst_uid = a->image_uid,
        .extent = {a->info.size.width, a->info.size.height},
        .layers = layers,
        .swap_rb = swap_rb,
        .per_eye_targets = fast_smaa,
        .dst_cleared = state.color_attachments[0].is_clear != 0,
    };
    // FAST bit 41: through the proxy. Bit off: the old raw borrow first (it drains the recorder
    // and keeps the rest of this command buffer inline), after which the proxy's calls are the
    // same direct calls on that command buffer. Under VR_GPU_PASSES, Record's GpuWorkBegin
    // borrows the raw handle either way.
    const RecorderProxy smaa_cmdbuf = scheduler.Commands();
    bool smaa_encoding = false;
    if (fast_smaa) {
        smaa_encoding = scheduler.RecorderEncoding();
    } else {
        (void)scheduler.CommandBuffer();
    }
    const bool smaa_recorded = vr_smaa->Record(smaa_cmdbuf, frame);
    NoteSmaaRecorder(fast_smaa, smaa_encoding, smaa_recorded, *vr_smaa);
    if (!smaa_recorded) {
        // The FXAA draw after all: its target back in the layout its render pass was set up with.
        a->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                   vk::AccessFlagBits2::eColorAttachmentWrite |
                       vk::AccessFlagBits2::eColorAttachmentRead,
                   {});
        VrAaNote(Refused, "SMAA could not record (see the VR smaa line)");
        return false;
    }
    VrAaNote(Smaa, fmt::format("aa_mode=smaa, {} eyes, {}x{} {}", layers, frame.extent.width, frame.extent.height,
                               vk::to_string(frame.dst_format)));
    return true;
}

void Rasterizer::VrTaaNoteRefusal(const std::string& why) {
    ++vr_taa.refused;
    vr_taa.facts.valid = false; // the next resolve starts over
    if (why != vr_taa.last_refusal) {
        vr_taa.last_refusal = why;
        if (++vr_taa.refusal_lines <= 20) {
            LOG_INFO(Render_Vulkan, "VR taa: refused ({}); SMAA this frame", why);
        }
    }
}

bool Rasterizer::VrTaaReplace(VideoCore::Image& b, VideoCore::Image& a, std::string* why) {
    const auto no = [&](const char* what) {
        *why = what;
        return false;
    };
    const auto& s = VR::GetSettings();
    // The camera update the frame was built with (as VrCaptureDepth picks it).
    VR::CameraRecord rec = VR::DrawFrameRecord();
    if (vr_dibr.first_draw_record != 0 && vr_dibr.first_draw_record != rec.id) {
        VR::CameraRecord first{};
        if (VR::GetRecord(vr_dibr.first_draw_record, first) && first.has_projection) {
            rec = first;
        }
    }
    if (rec.menu || !rec.dibr || !rec.has_projection || !rec.has_render_position) {
        return no("the frame's camera record is not a DIBR render with a projection");
    }
    const VrDepthCandidate* best = nullptr;
    for (const auto& candidate : vr_dibr.depth) {
        if (candidate.draws != 0 && (best == nullptr || candidate.draws > best->draws)) {
            best = &candidate;
        }
    }
    VideoCore::Image* depth = best ? texture_cache.TryGetImage(best->id, best->uid) : nullptr;
    if (depth == nullptr) {
        return no("no scene depth");
    }
    if (!best->zero_to_w || !(best->zmax > best->zmin) || depth->backing->num_samples != 1) {
        return no("the scene depth is not a single-sampled 0..W depth");
    }
    const vk::Extent2D extent{a.info.size.width, a.info.size.height};
    const vk::Extent2D depth_extent{depth->info.size.width, depth->info.size.height};
    if (depth_extent.width < extent.width || depth_extent.height < extent.height ||
        depth_extent.width > extent.width + extent.width / 16 ||
        depth_extent.height > extent.height + extent.height / 16) {
        return no("the scene depth is not the render's size");
    }
    if (!vr_render_taa) {
        vr_render_taa = std::make_unique<VrRenderTaa>(instance, scheduler);
    }
    if (!vr_render_taa->Ready()) {
        return no("the resolve is unavailable");
    }

    // This frame's camera, depth map and the reset guards against the last resolved frame.
    namespace RT = VR::RenderTaa;
    const float W = static_cast<float>(extent.width);
    const float H = static_cast<float>(extent.height);
    RT::Camera cam{};
    cam.rows = RT::CameraRows(rec.base_yaw - rec.head_yaw, -rec.head_pitch, rec.head_roll);
    cam.pos = {rec.render_position.x, rec.render_position.y, rec.render_position.z};
    RT::Focal(W, H, rec.proj_vfov, rec.proj_aspect, cam.fx, cam.fy);
    const RT::DepthAffine dmap = RT::MakeDepthAffine(rec.near_z, rec.far_z, best->zmin, best->zmax);
    RT::FrameFacts facts{};
    facts.valid = rec.near_z > 0.0f && rec.far_z > rec.near_z && rec.proj_vfov > 0.0f;
    facts.width = extent.width;
    facts.height = extent.height;
    facts.turn_serial = rec.turn_serial;
    facts.space_serial = rec.space_serial;
    facts.world_scale = rec.world_scale;
    facts.time_s = static_cast<double>(rec.tick_ns) * 1e-9;
    facts.rows = cam.rows;
    facts.pos = cam.pos;
    const RT::Reset reset = RT::ResetReason(vr_taa.facts, facts);
    if (!facts.valid) {
        return no("the frame's projection is not valid");
    }

    // The jitter this frame's scene draws carried, in window pixels of this render.
    const VR::TaaJitter jit = VR::FrameTaaJitter();
    float jx = 0.0f, jy = 0.0f;
    if (jit.on && jit.width > 0 && jit.height > 0) {
        jx = jit.px_x * (W / static_cast<float>(jit.width)) * (vr_taa.xscale < 0.0f ? -1.0f : 1.0f);
        // NDC y up: window y follows the viewport's y scale (negative: y down).
        jy = jit.px_y * (H / static_cast<float>(jit.height)) * (vr_taa.yscale < 0.0f ? -1.0f : 1.0f);
        ++vr_taa.jittered;
    }
    depth->Transit(vk::ImageLayout::eDepthStencilReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                   {});
    VideoCore::ImageViewInfo view_info{};
    view_info.format = vk::Format::eR32Sfloat; // the view swaps to the depth format and aspect
    const vk::ImageView depth_view = *depth->FindView(view_info, false).image_view;

    const bool history = reset == RT::Reset::None;
    VrRenderTaa::Frame frame{
        .src = b.GetImage(),
        .src_format = b.backing->image.image_ci.format,
        .src_uid = b.image_uid,
        .dst = a.GetImage(),
        .dst_format = a.backing->image.image_ci.format,
        .dst_uid = a.image_uid,
        .depth_view = depth_view,
        .depth_extent = extent, // a padded depth is read 1:1 from its top-left corner
        .extent = extent,
        .jitter_x = jx,
        .jitter_y = jy,
        .y_down = vr_taa.yscale < 0.0f,
        .reproj = history ? RT::MakeReprojector(cam, vr_taa.camera) : RT::Reprojector{},
        .fx = cam.fx,
        .fy = cam.fy,
        .pfx = history ? vr_taa.camera.fx : cam.fx,
        .pfy = history ? vr_taa.camera.fy : cam.fy,
        .depth = dmap,
        .prev_depth = history ? vr_taa.depth : dmap,
        .history = history,
        .alpha_min = s.taa_alpha_min,
        .gamma = s.taa_clip_gamma,
        .depth_reject = s.taa_depth_reject,
        .sharpen = s.taa_sharpen,
        .filter_px = s.taa_filter_px,
        .near_m = s.taa_near_m,
        .near_alpha = s.taa_near_alpha,
        .luma_weight = s.taa_luma_weight,
        .occluder = s.taa_occluder,
        .sharpen_lift = s.taa_sharpen_lift,
        .debug = static_cast<u32>(std::clamp(s.taa_debug, 0, 2)),
    };
    if (VR::Tune::Active()) {
        // VR_TUNE_FILE taa.alpha_min / taa.gamma / taa.depth_reject / taa.debug (tests).
        frame.alpha_min = std::clamp(VR::Tune::Get("taa.alpha_min", frame.alpha_min), 0.02f, 1.0f);
        frame.gamma = std::clamp(VR::Tune::Get("taa.gamma", frame.gamma), 0.25f, 4.0f);
        frame.depth_reject = VR::Tune::Get("taa.depth_reject", frame.depth_reject);
        frame.sharpen = std::clamp(VR::Tune::Get("taa.sharpen", frame.sharpen), 0.0f, 1.0f);
        // taa.filter_px / taa.near_m / taa.near_alpha (tests).
        frame.filter_px = VR::Tune::Get("taa.filter_px", frame.filter_px);
        frame.near_m = std::clamp(VR::Tune::Get("taa.near_m", frame.near_m), 0.0f, 12.0f);
        frame.near_alpha = std::clamp(VR::Tune::Get("taa.near_alpha", frame.near_alpha), 0.0f, 1.0f);
        // taa.luma_weight / taa.occluder (round 2 tests).
        frame.luma_weight =
            std::clamp(VR::Tune::Get("taa.luma_weight", frame.luma_weight), 0.0f, 1.0f);
        frame.occluder =
            VR::Tune::Get("taa.occluder", frame.occluder ? 1.0f : 0.0f) > 0.5f;
        frame.sharpen_lift =
            std::clamp(VR::Tune::Get("taa.sharpen_lift", frame.sharpen_lift), 0.0f, 0.5f);
        frame.debug = static_cast<u32>(
            std::clamp(VR::Tune::Get("taa.debug", static_cast<float>(frame.debug)), 0.0f, 2.0f));
        if (VR::Tune::Get("taa.jitter_sign", 1.0f) < 0.0f) {
            frame.jitter_x = -frame.jitter_x;
            frame.jitter_y = -frame.jitter_y;
        }
    }
    // taa_near_m (and its tune key) is in metres; the shader's depth is in game units.
    frame.near_m = RT::NearGameUnits(frame.near_m, s.world_scale);
    if (!vr_render_taa->Record(scheduler.CommandBuffer(), frame)) {
        return no("the resolve could not record (see the VR taa line)");
    }
    VR::NoteTaaResolve(extent.width, extent.height, true);
    vr_dibr.taa_resolved = true;
    vr_taa.facts = facts;
    vr_taa.camera = cam;
    vr_taa.depth = dmap;
    ++vr_taa.resolved;
    ++vr_taa.resets[static_cast<size_t>(reset)];
    VrAaNote(Taa, fmt::format("aa_mode=taa, {}x{}, {}", extent.width, extent.height,
                              jit.on ? fmt::format("jittered x{}", s.taa_phases)
                                     : std::string{"not jittered"}));
    if (++vr_taa.frames >= 600) {
        std::string resets;
        for (size_t i = 1; i < vr_taa.resets.size(); ++i) {
            if (vr_taa.resets[i] != 0) {
                resets += fmt::format(" {} {}", RT::ResetName(static_cast<RT::Reset>(i)),
                                      vr_taa.resets[i]);
            }
        }
        LOG_INFO(Render_Vulkan,
                 "VR taa: {} frames resolved ({} jittered, {} with history), {} refused (last: "
                 "{}); resets:{}; {}x{}, jitter ({:.2f}, {:.2f}) px, viewport y {}, history {} "
                 "MB; alpha_min {} gamma {} depth_reject {} sharpen {} filter {:.2f} px near {} (game units) "
                 "/ {} luma_weight {} occluder {} lift {} stable_depth {} debug {}",
                 vr_taa.resolved, vr_taa.jittered, vr_taa.resets[0], vr_taa.refused,
                 vr_taa.last_refusal.empty() ? std::string{"-"} : vr_taa.last_refusal,
                 resets.empty() ? std::string{" none"} : resets, extent.width, extent.height, jx,
                 jy, vr_taa.yscale < 0.0f ? "down" : "up", vr_render_taa->HistoryBytes() >> 20,
                 frame.alpha_min, frame.gamma, frame.depth_reject, frame.sharpen,
                 RT::FilterSigma(RT::FilterCode(frame.filter_px)), frame.near_m, frame.near_alpha,
                 frame.luma_weight, frame.occluder ? 1 : 0, frame.sharpen_lift,
                 s.taa_stable_depth ? 1 : 0,
                 frame.debug);
        const auto xs = vr_taa.xscale, ys = vr_taa.yscale;
        const auto facts_keep = vr_taa.facts;
        const auto cam_keep = vr_taa.camera;
        const auto depth_keep = vr_taa.depth;
        vr_taa = {};
        vr_taa.xscale = xs;
        vr_taa.yscale = ys;
        vr_taa.facts = facts_keep;
        vr_taa.camera = cam_keep;
        vr_taa.depth = depth_keep;
    }
    return true;
}

} // namespace Vulkan
