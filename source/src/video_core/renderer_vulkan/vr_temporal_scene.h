// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vr_camera_motion.h"
#include "video_core/renderer_vulkan/vr_object_motion.h"
#include "video_core/renderer_vulkan/vr_temporal_images.h"
#include "video_core/renderer_vulkan/vr_temporal_runtime.h"
#include <array>
#include <optional>
namespace Vulkan {
// One renderer-owned stereo frame. The rasterizer supplies scene-sized attachments,
// exact uploaded cameras and draw identities; output is exposed only as a complete pair.
class VrTemporalScene {
  public:
    VrTemporalScene(const Instance &instance, Scheduler &scheduler);
    using CameraPair = std::array<std::array<float, 216>, 2>;
    using Result = VrTemporalPair::Result;
    Result Begin(uint64_t frame, const VR::Graphics::Config &accepted,
                 const VrTemporalPair::Plan &plan, bool camera_cut = false);
    // FAST_GPU_S1_ASYM: a frame without Begin while the provider waits, configured, for its retry
    // (vr_temporal_failure.h). No objects, jitter, camera or output, as Begin with the provider
    // OFF leaves the frame; the provider and its history are kept (the next Begin is not
    // consecutive, so it restarts the history).
    void Pause() {
        active = false;
        published = false;
        have_camera = false;
        camera_bytes_valid = false;
        output = {};
    }
    bool Preparing() const { return runtime.Preparing(); }
    bool Active() const { return active; }
    VrObjectMotion *Objects() { return active && !attempted ? &objects : nullptr; }
    std::array<float, 2> Jitter() const {
        return active && !attempted ? jitter : std::array<float, 2>{};
    }
    // Called on the scene G-buffer's exact final per-eye camera upload, not UI or shadows.
    bool CaptureCamera(const CameraPair &data, float viewport_x_sign, float viewport_y_sign);
    void IncompleteMotion() { complete_motion = false; }
    Result Resolve(const std::array<VrTemporalImages::Source, 2> &sources, float frame_ms);
    std::optional<std::array<BbDlssImage, 2>> Output() const;
    bool Merge(const std::array<vk::ImageView, 2>& scene) { return published && images.Merge(scene); }
    const std::string &Problem() const { return problem.empty() ? runtime.Problem() : problem; }

  private:
    bool CaptureCameraFull(const CameraPair &data, float viewport_x_sign, float viewport_y_sign);
    VrTemporalRuntime runtime;
    VrTemporalImages images;
    VrObjectMotion objects;
    VrCameraMotion camera_motion;
    std::string problem;
    VR::Graphics::Config accepted;
    VrTemporalPair::Plan plan{};
    std::array<VrMotionCamera::Camera, 2> current{}, previous{};
    // Toggle bit 36: the bytes and viewport signs of a capture this frame that returned true
    // (Read of them gives `current`). Cleared together with have_camera.
    CameraPair camera_bytes{};
    float camera_x_sign{}, camera_y_sign{};
    bool camera_bytes_valid{};
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<BbDlssImage, 2> output{};
    uint64_t frame{}, last_frame{}, epoch{};
    Result begin_result{Result::Disabled};
    bool begun{}, active{}, have_camera{}, have_previous{}, complete_motion{}, attempted{},
        published{};
};
} // namespace Vulkan
