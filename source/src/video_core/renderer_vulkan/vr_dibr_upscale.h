// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <tsl/robin_map.h>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_depth_rescale.h"
#include "shader_recompiler/vr_motion.h"
#include "video_core/renderer_vulkan/vr_motion_history.h"

namespace Vulkan::DibrUpscale {
// DIBR with DLSS, FSR 4 or FSR 3.1.5 (graphics_settings.h DibrUpscaleSupports): the centre render,
// at the upscaler's input size since boot (memory_patcher.cpp, the S1 patch), is reconstructed
// once before the DIBR warp. Native stereo never enters it (its S1 reconstructs both eyes).
// VR_DIBR_UPSCALE=0 turns it off: nothing is allocated or loaded then.
inline bool Enabled() {
    static const bool enabled =
        !std::getenv("VR_NATIVE_STEREO") && VR::Graphics::DibrUpscaleAllowed();
    return enabled;
}
inline bool Requested() {
    return Enabled() && VR::Graphics::DibrUpscaleSupports(VR::Graphics::Accepted());
}
} // namespace Vulkan::DibrUpscale

namespace VR { struct CameraRecord; }
namespace VideoCore { struct Image; }
namespace Vulkan {
class Instance;
class Scheduler;
class GraphicsPipeline;
class VrObjectMotion;
// One provider feature (DLSS) or context (FSR) for the centre. It reads the tonemapped composite
// X as its stored bytes (like S1), the scene depth and motion: the camera's, and for characters
// and the player's body and held items their own (native stereo's motion variants, one view;
// the rasterizer's draw hooks in vk_rasterizer_temporal.cpp). The scene draws are jittered
// through the viewport (VR::SetUpscalerJitter).
class VrDibrUpscale {
public:
    VrDibrUpscale(const Instance&, Scheduler&);
    ~VrDibrUpscale();
    // The reconstructed centre as 8-bit UNORM bytes (X's encoding), readable by transfers in
    // `layout`. Empty: the frame keeps X.
    struct Output {
        vk::Image image{};
        vk::Extent2D extent{};
        vk::ImageLayout layout{vk::ImageLayout::eTransferSrcOptimal};
    };
    bool Ready() const;
    vk::Extent2D DesiredSize(vk::Extent2D input) const;
    Output Record(VideoCore::Image& color, VideoCore::Image& depth,
                  vk::ComponentMapping components, const VR::CameraRecord& camera,
                  float xsign, float ysign, bool already_resolved);
    // An immersive frame the capture could not give the provider (no usable depth): history
    // restarts at the next one.
    void Skip();
    // After this frame's reconstruction: our in-frame items go into it (VrInjectInFrame), sharp
    // and out of the provider's history, against `scene_depth` (the game's, input size) blitted
    // to the output size (nearest). Empty: they go into X as without the upscaler. ItemsDone
    // returns the scene to the capture's layout.
    struct ItemTarget {
        vk::ImageView color{}; // sRGB view, eColorAttachmentOptimal
        vk::Image depth{};     // eTransferSrcOptimal
        vk::Format depth_format{};
        vk::Extent2D extent{};
    };
    std::optional<ItemTarget> ItemsTarget(VideoCore::Image& scene_depth);
    void ItemsDone();
    // The input size of the last reconstruction (the scene depth the draw hooks look for).
    vk::Extent2D InputSize() const;
    // The draw hooks' frame state. BeginMotion at the frame's first scene draw; Objects is null
    // while the provider cannot resolve (no motion variants then).
    struct DrawState {
        Motion::IndexRangeCache index_ranges;
        std::span<const uint8_t> index_data;
        uint64_t index_address{};
        uint64_t frame{~0ull};
        uint32_t draws{}, motion_draws{}, refused{}, characters{}, player{};
        bool gbuffer{}, object_candidate{};
        // Toggle bit 36 (as native stereo's VrTemporalCapture::motion_variants): the pipeline
        // cache's non-null motion variants by base pipeline, for the same motion buffers.
        struct Variant {
            Shader::VrMotion motion{};
            const GraphicsPipeline* variant{};
        };
        tsl::robin_map<const GraphicsPipeline*, Variant> variants;
    } draws;
    void BeginMotion(uint64_t frame);
    VrObjectMotion* Objects();
private:
    bool ItemsRescale(VideoCore::Image& depth, VrDepthRescale::Mode mode);
    struct State;
    std::unique_ptr<State> state;
};
}
