// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_temporal_pair.h"
#include "video_core/texture_cache/image.h"
#include <array>
#include <bit>
#include <memory>
#include <string_view>
namespace VideoCore {
struct UniqueBuffer;
}
namespace Vulkan {
class Instance;
class Scheduler;
// Canonical, single-layer resources for each eye. Preparation preserves the actual
// scene render size: this class never downsamples a full-cost render to fake a preset.
class VrTemporalImages {
  public:
    VrTemporalImages(const Instance &instance, Scheduler &scheduler);
    ~VrTemporalImages();
    struct Source {
        vk::ImageView color{}, depth{}; // linear HDR colour, depth aspect, GENERAL
        uint32_t width{}, height{};     // exact source-view extent; caller owns transitions
        // Bit 46 (vr_dlss_direct.h): the images behind the views, their formats and the views'
        // array layer, so DLSS can read them in place. Without them the inputs are copied.
        vk::Image color_image{}, depth_image{};
        vk::Format color_format{}, depth_format{};
        uint32_t layer{};
    };
    // `direct` (bit 46, DLSS only): the provider reads `sources` itself and only the eyes in
    // `copies` (bit e = eye e, for the FAST_DLSS_DIRECT_VERIFY twin) get canonical colour/depth,
    // plus the layer-1 inputs FAST_DLSS_DIRECT_LAYER_COPY names (default: the right eye's depth,
    // which NGX does not read from its layer), which the provider then reads from the copies.
    // Otherwise, or when a source cannot be read in place, both eyes are copied as before.
    bool Prepare(const VR::Graphics::Config &accepted, bool provider_ready,
                 const VrTemporalPair::Plan &plan, const std::array<Source, 2> &sources,
                 bool direct, uint32_t copies);
    // Whether some of the prepared frame's colour/depth are the sources themselves, and the
    // copy traffic not issued per input pixel (colour 16, depth 8 bytes per eye not copied).
    bool Direct() const { return ready && direct; }
    uint32_t UncopiedBytesPerPixel() const {
        if (!ready || !direct)
            return 0u;
        return 16u * uint32_t(allocated.views - std::popcount(copied_color)) +
               8u * uint32_t(allocated.views - std::popcount(copied_depth));
    }
    std::array<BbDlssEvaluate, 2> Images() const;
    // Plan flags Fsr4VerifyTwin (FAST_UPSCALER_VERIFY, bit 37), Fsr4VerifyInterleave
    // (FAST_FSR4_INTERLEAVE_VERIFY, bit 45) and DlssVerifyTwin (FAST_DLSS_DIRECT_VERIFY,
    // bit 46): the twin's output.
    BbDlssImage VerifyOutput() const;
    // DlssVerifyTwin: `eye`'s canonical colour/depth and the twin's own motion target.
    BbDlssEvaluate TwinInputs(uint32_t eye) const;
    enum class VerifyKind { Fsr4Post, DlssDirect };
    // After a resolved frame whose twin ran: every `period` frames, read back `eye`'s output
    // and the twin's, and compare them once the GPU has finished (no waits).
    // `interleaved`: that frame's FSR eyes were recorded in lockstep (bit 45, for the log line).
    void Verify(uint64_t frame, uint32_t period, bool twin_evaluated, VerifyKind kind,
                uint32_t eye, bool interleaved = false);
    bool Merge(const std::array<vk::ImageView, 2>& scene);
    void DebugCapture(uint64_t frame, std::string_view metadata);
    void Release();

  private:
    struct Surface {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Format format{};
        uint32_t width{}, height{};
        bool initialized{};
    };
    using Eye = std::array<Surface, 4>; // colour, depth, motion, output
    bool Init();
    bool Allocate(const VrTemporalPair::Plan &plan);
    bool Create(Surface &surface, vk::Format format, uint32_t width, uint32_t height);
    static BbDlssImage Canonical(const Surface &surface);
    void CompareVerify();
    void ReleaseVerify();
    const Instance &instance;
    Scheduler &scheduler;
    std::array<Eye, 2> eyes;
    Surface verify;      // twin output: Fsr4VerifyTwin, Fsr4VerifyInterleave or DlssVerifyTwin
    Surface twin_motion; // DlssVerifyTwin: motion vectors from the copied depth
    std::unique_ptr<VideoCore::UniqueBuffer> verify_download; // retired with a deferred op
    void *verify_mapped{};
    uint64_t verify_ticket{}, verify_frame{};
    uint32_t verify_eye{};
    VerifyKind verify_kind{VerifyKind::Fsr4Post};
    bool verify_pending{}, verify_direct{}, verify_interleaved{};
    VrTemporalPair::Plan allocated{};
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline, merge_pipeline;
    // Bit 46: this frame's sources when the provider reads them in place.
    std::array<Source, 2> direct_sources;
    // Eyes whose colour/depth were copied into canonical images this frame (bit e = eye e), and
    // eyes whose colour/depth the provider reads in place (bit 46; a copied input of the
    // verify twin's eye may still be read in place by the eye itself).
    uint32_t copied_color{}, copied_depth{}, direct_color{}, direct_depth{};
    mutable std::array<bool, 2> direct_logged{}; // bit 46: each eye's inputs logged once
    bool failed{}, ready{}, direct{};
};
} // namespace Vulkan
