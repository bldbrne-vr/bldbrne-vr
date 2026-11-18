// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "video_core/renderer_vulkan/vr_temporal_scene.h"
#include "video_core/renderer_vulkan/vr_fsr4_interleave.h"
#include "video_core/renderer_vulkan/vr_dlss_direct.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/render_diagnostic.h"
#include "video_core/renderer_vulkan/vr_temporal_optimizations.h"
#include "core/vr/tonitrus_charge.h"
namespace Vulkan {
namespace {
float Halton(uint32_t n, uint32_t base) {
    float value = 0, step = 1;
    while (n) {
        step /= float(base);
        value += step * float(n % base);
        n /= base;
    }
    return value;
}
} // namespace
VrTemporalScene::VrTemporalScene(const Instance &i, Scheduler &s)
    : runtime(i, s), images(i, s), objects(i, s), camera_motion(i, s), focus_pass(i, s) {}
VrTemporalScene::Result VrTemporalScene::Begin(uint64_t id, const VR::Graphics::Config &config,
                                               const VrTemporalPair::Plan &next, bool cut, bool probe) {
    // Do not apply a menu acceptance midway through a stereo frame or begin history twice.
    if (begun && id == frame)
        return begin_result;
    if (begun && id < frame) {
        active = false;
        published = false;
        have_previous = false;
        runtime.ResetHistory();
        return begin_result = Result::Invalid;
    }
    recovery_probe = probe;
    cut = cut || recovery_probe;
    const bool consecutive = begun && frame != UINT64_MAX && id == frame + 1;
    if (!consecutive || cut || next != plan || config.upscaler != accepted.upscaler ||
        VR::Graphics::EffectiveProfile(config) != VR::Graphics::EffectiveProfile(accepted)) {
        have_previous = false;
        runtime.ResetHistory();
    }
    problem.clear();
    frame = id;
    begun = true;
    accepted = config;
    plan = next;
    active = false;
    have_camera = false;
    camera_bytes_valid = false;
    valid_scene = true;
    attempted = false;
    published = false;
    output = {};
    const auto result = runtime.Begin(accepted, plan);
    if (result != Result::Ready) {
        objects.Begin(accepted, false, frame, runtime.Epoch());
        images.Release();
        have_previous = false;
        return begin_result = result;
    }
    // Native multiview renders one size for both eyes. Reject unsupported reverse-depth
    // cameras rather than feeding the normal-depth motion shader incorrect reprojection.
    if (plan.inverted || plan.eyes[0].input_width != plan.eyes[1].input_width ||
        plan.eyes[0].input_height != plan.eyes[1].input_height) {
        have_previous = false;
        return begin_result = Result::Invalid;
    }
    if (epoch != runtime.Epoch())
        have_previous = false;
    epoch = runtime.Epoch();
    // Object history is optional. A busy history slot or unavailable buffers must not
    // stop reconstruction: PrepareRead returns no stale targets while objects is inactive,
    // and the composer generates camera-only vectors for both eyes.
    objects.Begin(accepted, true, frame, epoch, cut);
    const auto &size = plan.eyes[0];
    const uint32_t phases = Motion::JitterPhases(size.input_width, size.output_width);
    const uint32_t sample = uint32_t(frame % phases) + 1;
    jitter = recovery_probe ? std::array<float, 2>{}
                            : std::array<float, 2>{Halton(sample, 2) - .5f,
                                                   Halton(sample, 3) - .5f};
    active = true;
    return begin_result = Result::Ready;
}
// Toggle bit 36: BindResources calls this for the 864-byte scene constants of every G-buffer
// draw, and nearly all of them upload the same final camera pair. Read depends only on the bytes
// and the viewport signs, so a pair equal to one whose capture returned true this frame gives
// `current` again and the comparison succeeds: return true without parsing and inverting.
bool VrTemporalScene::CaptureCamera(const CameraPair &data, float xs, float ys) {
    return CaptureCameraImpl(&data,
        CameraBytes{reinterpret_cast<const std::byte*>(data[0].data()), sizeof(data[0])},
        CameraBytes{reinterpret_cast<const std::byte*>(data[1].data()), sizeof(data[1])}, xs, ys);
}
bool VrTemporalScene::CaptureCameraBytes(CameraBytes left, CameraBytes right, float xs, float ys) {
    // Compare immediately; no input pointers survive this call. Full parsing still owns a copy.
    return CaptureCameraImpl(nullptr, left, right, xs, ys);
}
bool VrTemporalScene::CaptureCameraImpl(const CameraPair* owned, CameraBytes left,
                                      CameraBytes right, float xs, float ys) {
    const auto full_capture = [&] {
        if (owned)
            return CaptureCameraFull(*owned, xs, ys);
        CameraPair data;
        std::memcpy(data[0].data(), left.data(), left.size());
        std::memcpy(data[1].data(), right.data(), right.size());
        return CaptureCameraFull(data, xs, ys);
    };
    if (!FastRender::On(MotionFast))
        return full_capture();
    if (!active || attempted)
        return false;
    ++g_motion_fast.camera_calls;
    if (camera_bytes_valid && xs == camera_x_sign && ys == camera_y_sign &&
        (owned ? std::memcmp(owned, &camera_bytes, sizeof(CameraPair)) == 0
               : std::memcmp(left.data(), camera_bytes[0].data(), left.size()) == 0 &&
                     std::memcmp(right.data(), camera_bytes[1].data(), right.size()) == 0)) {
        ++g_motion_fast.camera_reused;
        if (!MotionVerifySample())
            return true;
        const bool full = full_capture();
        if (!full) {
            ++g_motion_fast.verify_mismatches;
            camera_bytes_valid = false;
            LOG_ERROR(Render_Vulkan, "FAST motion verify: reused scene camera refused: {}",
                      problem);
        }
        return full;
    }
    const bool captured = full_capture();
    if (captured) {
        if (owned) {
            camera_bytes = *owned;
        } else {
            std::memcpy(camera_bytes[0].data(), left.data(), left.size());
            std::memcpy(camera_bytes[1].data(), right.data(), right.size());
        }
        camera_x_sign = xs;
        camera_y_sign = ys;
        camera_bytes_valid = true;
    }
    return captured;
}
bool VrTemporalScene::CaptureCameraFull(const CameraPair &data, float xs, float ys) {
    if (!active || attempted)
        return false;
    std::array<VrMotionCamera::Camera, 2> pair;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto camera = VrMotionCamera::Read(data[eye], xs, ys);
        if (!camera) {
            problem = "invalid captured scene camera";
            valid_scene = false;
            return false;
        }
        pair[eye] = *camera;
    }
    // Multiple scene cameras in one frame need separate histories. Refuse ambiguous
    // captures instead of silently pairing the last camera with earlier geometry/depth.
    if (have_camera) {
        for (uint32_t eye = 0; eye < 2; ++eye)
            if (pair[eye].view != current[eye].view ||
                pair[eye].projection != current[eye].projection ||
                pair[eye].offset != current[eye].offset) {
#ifndef BBVR_NO_LOGS
                static const bool trace = [] {
                    const char* value = std::getenv("VR_TEMPORAL_TRACE");
                    return value && *value && *value != '0';
                }();
                if (trace && problem != "multiple different scene cameras in one frame") {
                    const auto delta = [](const auto& a, const auto& b) {
                        double largest = 0.0;
                        for (size_t i = 0; i < a.size(); ++i)
                            largest = std::max(largest, std::abs(double(a[i]) - double(b[i])));
                        return largest;
                    };
                    LOG_INFO(Render_Vulkan,
                             "VR temporal camera conflict: frame {} eye {} max absolute delta "
                             "view={:.9g} projection={:.9g} offset={:.9g}",
                             frame, eye, delta(pair[eye].view, current[eye].view),
                             delta(pair[eye].projection, current[eye].projection),
                             delta(pair[eye].offset, current[eye].offset));
                }
#endif
                problem = "multiple different scene cameras in one frame";
                valid_scene = false;
                return false;
            }
        return true;
    }
    current = pair;
    have_camera = true;
    return true;
}
VrTemporalScene::Result
VrTemporalScene::Resolve(const std::array<VrTemporalImages::Source, 2> &sources, float frame_ms) {
    if (!active || attempted)
        return Result::Pending;
    attempted = true;
    if (!have_camera || !valid_scene || !std::isfinite(frame_ms) || frame_ms <= 0) {
        if (problem.empty()) problem = !have_camera ? "scene camera not captured" :
            !valid_scene ? "invalid scene camera/depth" : "invalid frame interval";
        runtime.ResetHistory();
        have_previous = false;
        return Result::Invalid;
    }
    const auto &size = plan.eyes[0];
    auto object_views = objects.PrepareRead(size.input_width, size.input_height);
    // A static-only scene legitimately has no object attachment this frame.
    // PrepareRead never returns stale targets; use camera vectors for both eyes.
    const bool have_objects = object_views[0] && object_views[1];
    VrTemporalPair::Frame provider_frame;
    // The same near/far planes must describe both eyes; asymmetric projection changes XY only.
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto &p = current[eye].projection;
        if (!(p[2] >= 1 && p[3] < 0) || p[2] != current[0].projection[2] ||
            p[3] != current[0].projection[3]) {
            problem = "unsupported or mismatched eye depth projection";
            runtime.ResetHistory();
            have_previous = false;
            return Result::Invalid;
        }
        provider_frame.vertical_fov[eye] = 2 * std::atan(1 / std::abs(p[1]));
    }
    provider_frame.near_plane = -current[0].projection[3] / current[0].projection[2];
    provider_frame.far_plane = current[0].projection[2] > 1
                                   ? -current[0].projection[3] / (current[0].projection[2] - 1)
                                   : std::numeric_limits<float>::max();
    // Bit 46: plain DLSS reads the scene's own layer views (vr_dlss_direct.h). The
    // FAST_DLSS_DIRECT_VERIFY twin's eye is still copied, for the twin.
    const bool dlss = accepted.upscaler == VR::Graphics::Upscaler::Dlss;
    const bool direct_bit = dlss && FastRender::On(DlssDirect); // read once for this frame
    const bool twin = dlss && (plan.provider_flags & VrTemporalPair::DlssVerifyTwin) != 0;
    const uint32_t twin_eye = VrTemporalPair::DlssTwinEye(plan.provider_flags);
    if (!images.Prepare(accepted, runtime.CanProduceMotion(), plan, sources,
                        direct_bit && DlssDirectEligible(dlss, accepted.dlss5),
                        twin ? 1u << twin_eye : 0u)) {
        problem = "canonical eye image preparation failed";
        runtime.ResetHistory();
        have_previous = false;
        return Result::Failed;
    }
    provider_frame.eyes = images.Images();
    provider_frame.direct_inputs = images.Direct();
    // Empty unless FAST_UPSCALER_VERIFY (FSR) or FAST_DLSS_DIRECT_VERIFY (DLSS).
    provider_frame.verify_output = images.VerifyOutput();
    if (twin) {
        provider_frame.verify_inputs = images.TwinInputs(twin_eye);
        provider_frame.verify_eye = twin_eye;
    }
    provider_frame.fsr4_pair = FastRender::On(Fsr4Interleave);
    const bool verify_interleave = (plan.provider_flags & VrTemporalPair::Fsr4VerifyInterleave) != 0;
    bool reset = !have_previous || last_frame == UINT64_MAX || last_frame + 1 != frame;
    // Tonitrus' strike: a frame where its world flash jumps (the flicker's on, its dip, its second
    // flash) starts the history afresh, or the upscaler and the TAA would average the jump away;
    // through its smooth fade the history is kept (core/vr/tonitrus_charge.h FlashJumped). The
    // level is this frame's own (SetFlashLevel), not the clock's at this resolve.
    if (VR::TonitrusCharge::FlashJumped(flash_level, flash_next))
        reset = true;
    flash_level = flash_next;
    // FAST_FSR4_INTERLEAVE_VERIFY_RESET (diagnostic): the eyes and the twin restart together
    // halfway between comparisons, so each comparison covers only frames since that reset.
    if (verify_interleave && Fsr4InterleaveVerifyReset() &&
        frame % Fsr4InterleaveVerifyPeriod() == Fsr4InterleaveVerifyPeriod() / 2)
        reset = true;
    if (reset)
        runtime.ResetHistory();
    // FAST_UPSCALER_VERIFY_RESET (diagnostic): both eyes and the twin (which copies the left
    // eye's frame) restart together half a period before each comparison. FSR ignores its
    // history and recurrent state on a reset frame, so both start from the same state. Only
    // while the twin still runs: without comparisons the restarts would just cost history.
    const auto verify_period = UpscalerFastVerifyPeriod();
    const bool lockstep = provider_frame.verify_output.image && verify_period &&
                          UpscalerFastVerifyLockstep() && runtime.VerifyTwinValid() &&
                          frame % verify_period == verify_period / 2;
    std::array<VrCameraMotion::Eye, 2> eyes;
    std::optional<VrCameraMotion::Eye> twin_motion;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto &f = provider_frame.eyes[eye];
        f.jitter_x = jitter[0];
        f.jitter_y = jitter[1];
        f.frame_ms = frame_ms;
        f.reset = reset || lockstep;
        const auto constants = VrMotionCamera::Build(
            current[eye], reset ? current[eye] : previous[eye], size.input_width, size.input_height,
            jitter, reset ? jitter : previous_jitter, have_objects);
        eyes[eye] = {f.depth.view, object_views[eye], f.motion.view, f.motion.image, constants};
        // The twin's vectors come from the copied depth, as the old path's did.
        if (const auto &t = provider_frame.verify_inputs; twin && eye == twin_eye && t.motion.image)
            twin_motion = VrCameraMotion::Eye{t.depth.view, object_views[eye], t.motion.view,
                                              t.motion.image, constants};
    }
    if (!camera_motion.Record(accepted, runtime.CanProduceMotion(), eyes,
                              twin_motion ? &*twin_motion : nullptr)) {
        problem = "camera/object motion composition failed";
        runtime.ResetHistory();
        have_previous = false;
        return Result::Failed;
    }
    // DLSS focus: the pair made each eye's feature its rectangle's size, so DLSS gets the
    // rectangles cut out into images of their own (origin at the corner, as any whole eye: the
    // same for both eyes), and the whole eyes are composed after it. A failure skips this frame
    // and turns focus off from the next (whole-eye DLSS again).
    const bool focus = runtime.FocusOn();
    const auto whole = provider_frame.eyes;
    if (focus) {
        if (!focus_pass.Crop(provider_frame.eyes, plan.focus)) {
            DlssFocus::Off("the rectangles could not be cut out");
            problem = "DLSS focus: the rectangles could not be cut out";
            runtime.ResetHistory();
            have_previous = false;
            return Result::Invalid;
        }
        provider_frame.direct_inputs = false; // the cut-outs are this frame's inputs
        // FSR rebuilds view positions from the vertical field of view and the input's aspect:
        // the cut-out's own (its share of the eye's height), not the whole eye's.
        for (uint32_t eye = 0; eye < 2; ++eye)
            provider_frame.vertical_fov[eye] =
                2 * std::atan(std::tan(provider_frame.vertical_fov[eye] / 2) *
                              float(plan.focus[eye].in_h) / float(size.input_height));
    }
    const auto result = runtime.Resolve(frame, provider_frame);
    if (result == Result::Ready && focus) {
        std::array<VrDlssFocusPass::Eye, 2> composed{};
        for (uint32_t eye = 0; eye < 2; ++eye)
            composed[eye] = {sources[eye].color, vk::ImageView{whole[eye].motion.view},
                             provider_frame.eyes[eye].output, whole[eye].output, plan.focus[eye]};
        std::array<BbDlssImage, 2> written{};
        if (!focus_pass.Compose(composed, size.input_width, size.input_height, jitter,
                                reset || lockstep, frame, written)) {
            DlssFocus::Off("the eyes could not be composed");
            problem = "DLSS focus: the eyes could not be composed";
            runtime.ResetHistory();
            have_previous = false;
            return Result::Invalid;
        }
        for (uint32_t eye = 0; eye < 2; ++eye)
            provider_frame.eyes[eye].output = written[eye]; // published below
    }
    previous = current;
    previous_jitter = jitter;
    last_frame = frame;
    have_previous = result == Result::Ready;
    if (result == Result::Ready) {
        for (uint32_t eye = 0; eye < 2; ++eye)
            output[eye] = provider_frame.eyes[eye].output;
        published = true;
        // FSR 4.1: what its bridge runs (AMD's 4.1, or FSR 3.1.5 in its place and why), in the
        // log and the play build's diagnostics whenever it changes.
        if (auto status = runtime.FsrBridgeStatus(); !status.empty() && status != bridge_status) {
            bridge_status = std::move(status);
            LOG_INFO(Render_Vulkan, "FSR 4.1 bridge: {}", bridge_status);
            Common::RenderDiagnostic::Write("FSR 4.1 bridge: " + bridge_status);
        }
        if (dlss && !focus) { // focus reads the eyes into its cut-outs: neither count applies
            // Bit 46 counters: an input read in place did not move its bytes (colour 16, depth 8
            // per input pixel and eye).
            const uint64_t pixels = uint64_t(size.input_width) * size.input_height;
            if (provider_frame.direct_inputs) {
                DlssDirectNote(g_dlss_direct.direct);
                DlssDirectNote(g_dlss_direct.saved_bytes, pixels * images.UncopiedBytesPerPixel());
            } else {
                DlssDirectNote(g_dlss_direct.copied);
                if (direct_bit) // DLSS 5, a capture directory or an unexpected source format
                    DlssDirectNote(g_dlss_direct.fallback);
            }
        }
        if (provider_frame.verify_output.image) {
            if (twin)
                images.Verify(frame, DlssDirectVerifyPeriod(), runtime.DlssTwinCurrent(),
                              VrTemporalImages::VerifyKind::DlssDirect, twin_eye);
            else
                images.Verify(frame,
                              verify_interleave ? Fsr4InterleaveVerifyPeriod()
                                                : UpscalerFastVerifyPeriod(),
                              runtime.VerifyTwinValid(), VrTemporalImages::VerifyKind::Fsr4Post,
                              0, runtime.FsrPairInterleaved());
        }
        static const bool capture = std::getenv("VR_TEMPORAL_CAPTURE_DIR") != nullptr;
        if (capture) {
            nlohmann::json meta{{"frame",frame},{"reset",reset},{"jitter",jitter},
                {"recovery_probe",recovery_probe},
                {"input",{size.input_width,size.input_height}},
                {"output",{size.output_width,size.output_height}},
                {"provider",int(accepted.upscaler)},{"dlss5",accepted.dlss5}};
            for (unsigned eye=0;eye<2;++eye)
                meta["cameras"].push_back({{"view",current[eye].view},
                    {"projection",current[eye].projection},{"offset",current[eye].offset}});
            images.DebugCapture(frame,meta.dump(2));
        }
    }
    return result;
}
std::optional<std::array<BbDlssImage, 2>> VrTemporalScene::Output() const {
    if (!published)
        return {};
    return output;
}
} // namespace Vulkan
