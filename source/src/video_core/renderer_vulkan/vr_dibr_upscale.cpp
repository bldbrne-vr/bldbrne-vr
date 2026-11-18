// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_dibr_upscale.h"
#include <cmath>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "core/vr/graphics_status.h"
#include "core/vr/render_taa.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_camera_motion.h"
#include "video_core/renderer_vulkan/vr_depth_rescale.h"
#include "video_core/renderer_vulkan/vr_dlss_direct.h"
#include "video_core/renderer_vulkan/vr_dlss_preset.h"
#include "video_core/renderer_vulkan/vr_object_motion.h"
#include "video_core/renderer_vulkan/vr_temporal_images.h"
#include "video_core/renderer_vulkan/vr_temporal_runtime.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"

namespace Vulkan {
namespace {
// The output: the size the boot patch reconstructs to (S1Output, the headset's centre size) when
// the guest renders below it at the same aspect; else the render's own size (DLAA / native AA,
// e.g. a template the patch did not recognise).
vk::Extent2D OutputSize(vk::Extent2D input) {
    const auto boot = VR::Graphics::S1Output();
    if (boot.width < input.width || boot.height < input.height || boot.width > 8192 ||
        boot.height > 8192)
        return input;
    const double aspect = double(boot.width) * input.height / (double(boot.height) * input.width);
    return std::abs(aspect - 1.0) <= 0.01 ? vk::Extent2D{boot.width, boot.height} : input;
}
// FSR 4's performance options, as the native pair asks for them (bit 37); none for FSR 3.1.5.
uint32_t ProviderFlags(const VR::Graphics::Config& c) {
    if (c.upscaler != VR::Graphics::Upscaler::Fsr4 || !FastRender::On(UpscalerFast))
        return 0;
    uint32_t flags = UpscalerFastOn(UpscalerFastPart::Fsr4Post) ? VrTemporalPair::Fsr4OptimizedPost : 0u;
    if (UpscalerFastSpdFence())
        flags |= VrTemporalPair::Fsr4FencedSpd;
    return flags;
}
const char* ProviderName(const VR::Graphics::Config& c) {
    return VR::Graphics::UpscalerName(c.upscaler);
}
} // namespace

struct VrDibrUpscale::State {
    const Instance& instance;
    Scheduler& scheduler;
    VrTemporalRuntime runtime;
    VrTemporalImages images;
    VrCameraMotion motion;
    VrObjectMotion objects;
    vk::Extent2D input{};
    bool objects_active{};
    VrMotionCamera::Camera previous{};
    std::array<float, 2> previous_jitter{};
    VR::RenderTaa::FrameFacts previous_facts{};
    uint64_t frame{}, resolved{}, skipped{}, refused{};
    bool ready{}, logged{}, failed_logged{};
    // The reconstructed centre, output size, 8-bit UNORM (X's encoding): DLSS's 8-bit output is
    // copied, FSR's half floats blitted (no sRGB step: the values already are X's encoding).
    // Mutable, with an sRGB view, so our in-frame items are drawn into it (ItemsTarget); the
    // scene depth blitted to its size (nearest) is their depth.
    VideoCore::UniqueImage scene;
    vk::UniqueImageView scene_srgb;
    VideoCore::UniqueImage depth_out;
    std::array<vk::UniqueImageView, 1> depth_targets; // for the rescale's draw (AMD)
    std::unique_ptr<VrDepthRescale> rescale;          // no depth blits (AMD): draw or copy
    vk::Format depth_format{};
    vk::Extent2D scene_size{};
    vk::ImageLayout scene_layout{vk::ImageLayout::eUndefined};
    State(const Instance& i, Scheduler& s)
        : instance(i), scheduler(s), runtime(i, s), images(i, s), motion(i, s), objects(i, s) {
        objects.SetViews(1);
    }
    ~State() { ReleaseScene(); }
    void ReleaseScene() {
        if (scene || depth_out)
            scheduler.DeferOperation([old = std::move(scene), view = std::move(scene_srgb),
                                      depth = std::move(depth_out),
                                      targets = std::move(depth_targets)]() mutable {
                view.reset();
                for (auto& target : targets) target.reset();
                old.Destroy();
                depth.Destroy();
            });
        depth_targets = {};
        scene = VideoCore::UniqueImage();
        depth_out = VideoCore::UniqueImage();
        scene_srgb.reset();
        scene_size = vk::Extent2D{};
        depth_format = vk::Format::eUndefined;
        scene_layout = vk::ImageLayout::eUndefined;
    }
    void Off() {
        auto off = VR::Graphics::Accepted();
        off.upscaler = VR::Graphics::Upscaler::Off;
        runtime.Begin(off, {});
        images.Release();
        ReleaseScene();
        objects.Begin(off, false, 0, 0); // releases its buffers
        objects_active = false;
        previous_facts.valid = false;
        ready = false;
        VR::SetUpscalerJitter(0);
    }
    bool Scene(vk::Extent2D size) {
        if (scene_size == size && scene) return true;
        ReleaseScene();
        scene = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        scene.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {size.width, size.height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined});
        if (!scene) return false;
        auto view = instance.GetDevice().createImageViewUnique(
            {.image = scene.image, .viewType = vk::ImageViewType::e2D,
             .format = vk::Format::eR8G8B8A8Srgb,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
        if (view.result != vk::Result::eSuccess) {
            ReleaseScene();
            return false;
        }
        scene_srgb = std::move(view.value);
        scene_size = size;
        scene_layout = vk::ImageLayout::eUndefined;
        return true;
    }
    void SceneTo(vk::ImageLayout layout, vk::PipelineStageFlags2 stage, vk::AccessFlags2 access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = stage, .dstAccessMask = access,
            .oldLayout = scene_layout, .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = scene.image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        scene_layout = layout;
    }
    // The provider's output (GENERAL) into the scene: a copy for 8-bit, a blit for half floats.
    bool Store(vk::Image source, vk::Format format, vk::Extent2D size) {
        if (!Scene(size)) return false;
        scene_layout = vk::ImageLayout::eUndefined; // every texel is replaced
        SceneTo(vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
        const auto cmd = scheduler.CommandBuffer();
        if (format == vk::Format::eR8G8B8A8Unorm) {
            const vk::ImageCopy copy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .extent = {size.width, size.height, 1}};
            cmd.copyImage(source, vk::ImageLayout::eGeneral, scene.image,
                          vk::ImageLayout::eTransferDstOptimal, copy);
        } else {
            const std::array<vk::Offset3D, 2> offsets{vk::Offset3D{0, 0, 0},
                vk::Offset3D{int32_t(size.width), int32_t(size.height), 1}};
            const vk::ImageBlit blit{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = offsets, .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = offsets};
            cmd.blitImage(source, vk::ImageLayout::eGeneral, scene.image,
                          vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
        }
        SceneTo(vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferRead);
        return true;
    }
};

VrDibrUpscale::VrDibrUpscale(const Instance& i, Scheduler& s) : state(std::make_unique<State>(i, s)) {}
VrDibrUpscale::~VrDibrUpscale() = default;
bool VrDibrUpscale::Ready() const { return DibrUpscale::Requested() && state->ready; }
vk::Extent2D VrDibrUpscale::DesiredSize(vk::Extent2D input) const {
    return Ready() ? OutputSize(input) : input;
}
vk::Extent2D VrDibrUpscale::InputSize() const { return state->input; }
std::optional<VrDibrUpscale::ItemTarget> VrDibrUpscale::ItemsTarget(VideoCore::Image& depth) {
    auto& s = *state;
    if (!s.scene || s.scene_layout != vk::ImageLayout::eTransferSrcOptimal ||
        depth.backing->num_samples != 1)
        return {};
    const vk::Format format = depth.backing->image.image_ci.format;
    // AMD cannot blit depth: VrDepthRescale draws it there (or copies it through a buffer).
    const auto mode = VrDepthRescale::Choose(s.instance, depth, vk::ImageAspectFlagBits::eDepth);
    if (mode == VrDepthRescale::Mode::None)
        return {}; // the items then go into the game's frame (VrInjectInFrame)
    if (!s.depth_out || s.depth_format != format) {
        if (s.depth_out)
            s.scheduler.DeferOperation([old = std::move(s.depth_out),
                                        targets = std::move(s.depth_targets)]() mutable {
                for (auto& target : targets) target.reset();
                old.Destroy();
            });
        s.depth_targets = {};
        s.depth_out = VideoCore::UniqueImage(s.instance.GetDevice(), s.instance.GetAllocator());
        s.depth_out.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D, .format = format,
            .extent = {s.scene_size.width, s.scene_size.height, 1}, .mipLevels = 1,
            .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                     (mode == VrDepthRescale::Mode::Blit
                          ? vk::ImageUsageFlags{}
                          : vk::ImageUsageFlags{vk::ImageUsageFlagBits::eDepthStencilAttachment}),
            .initialLayout = vk::ImageLayout::eUndefined});
        if (!s.depth_out) {
            s.depth_format = vk::Format::eUndefined;
            return {};
        }
        s.depth_format = format;
    }
    s.scheduler.EndRendering();
    if (mode != VrDepthRescale::Mode::Blit && !ItemsRescale(depth, mode))
        return {}; // nothing recorded: the items go into the game's frame
    if (mode != VrDepthRescale::Mode::Blit) {
        s.SceneTo(vk::ImageLayout::eColorAttachmentOptimal,
                  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                  vk::AccessFlagBits2::eColorAttachmentRead |
                      vk::AccessFlagBits2::eColorAttachmentWrite);
        return ItemTarget{*s.scene_srgb, s.depth_out.image, format, s.scene_size};
    }
    depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    const auto cmd = s.scheduler.CommandBuffer();
    const bool stencil = format == vk::Format::eD32SfloatS8Uint ||
                         format == vk::Format::eD24UnormS8Uint ||
                         format == vk::Format::eD16UnormS8Uint;
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s.depth_out.image,
        .subresourceRange = {stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                                     : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth},
                             0, 1, 0, 1}};
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
            vk::Offset3D{int32_t(depth.info.size.width), int32_t(depth.info.size.height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
            vk::Offset3D{int32_t(s.scene_size.width), int32_t(s.scene_size.height), 1}}};
    cmd.blitImage(depth.GetImage(), vk::ImageLayout::eTransferSrcOptimal, s.depth_out.image,
                  vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    s.SceneTo(vk::ImageLayout::eColorAttachmentOptimal,
              vk::PipelineStageFlagBits2::eColorAttachmentOutput,
              vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    return ItemTarget{*s.scene_srgb, s.depth_out.image, format, s.scene_size};
}
// No depth blits (AMD): the scene depth into depth_out by VrDepthRescale's draw (sampled, as its
// attachment) or buffer copy, depth_out left in TRANSFER_SRC_OPTIMAL like the blit leaves it.
bool VrDibrUpscale::ItemsRescale(VideoCore::Image& depth, VrDepthRescale::Mode mode) {
    auto& s = *state;
    const vk::Format format = s.depth_format;
    const bool draw = mode == VrDepthRescale::Mode::Draw;
    constexpr auto ds_stages = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
    if (draw)
        depth.Transit(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                      vk::AccessFlagBits2::eShaderRead, {});
    else
        depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    const auto cmd = s.scheduler.Commands();
    const bool stencil = format == vk::Format::eD32SfloatS8Uint ||
                         format == vk::Format::eD24UnormS8Uint ||
                         format == vk::Format::eD16UnormS8Uint;
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = draw ? ds_stages : vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = draw ? vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                              : vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = draw ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                          : vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s.depth_out.image,
        .subresourceRange = {stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                                     : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth},
                             0, 1, 0, 1}};
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    if (!s.rescale)
        s.rescale = std::make_unique<VrDepthRescale>(s.instance, s.scheduler);
    const vk::Extent2D src_size{depth.info.size.width, depth.info.size.height};
    const bool scaled =
        draw ? s.rescale->Draw(cmd, depth, src_size, s.depth_out.image, s.scene_size, 1, format,
                               vk::ImageAspectFlagBits::eDepth, s.depth_targets, {})
             : s.rescale->Record(cmd, depth.GetImage(), src_size, s.depth_out.image, s.scene_size,
                                 1, format, vk::ImageAspectFlagBits::eDepth);
    if (draw) // the game's depth as the blit leaves it
        depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    if (!scaled)
        return false;
    barrier.srcStageMask = draw ? ds_stages : vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = draw ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                 : vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    return true;
}
void VrDibrUpscale::ItemsDone() {
    auto& s = *state;
    if (s.scene && s.scene_layout != vk::ImageLayout::eTransferSrcOptimal)
        s.SceneTo(vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eTransferRead);
}
void VrDibrUpscale::BeginMotion(uint64_t frame) {
    auto& s = *state;
    // While the provider can resolve (a skipped frame keeps the buffers); a frame after a cut or a
    // skipped one restarts the position history.
    s.objects_active = s.objects.Begin(VR::Graphics::Accepted(), s.runtime.CanProduceMotion(), frame,
                                       s.runtime.Epoch(), !s.previous_facts.valid);
}
VrObjectMotion* VrDibrUpscale::Objects() {
    return state->objects_active && state->objects.Active() ? &state->objects : nullptr;
}
void VrDibrUpscale::Skip() {
    auto& s = *state;
    s.previous_facts.valid = false;
    ++s.skipped;
}
VrDibrUpscale::Output VrDibrUpscale::Record(VideoCore::Image& color, VideoCore::Image& depth,
        vk::ComponentMapping components, const VR::CameraRecord& rec,
        float xs, float ys, bool already_resolved) {
    auto& s = *state;
    const auto config = VR::Graphics::Accepted();
    if (!DibrUpscale::Requested()) {
        if (s.ready || s.scene || s.previous_facts.valid) s.Off();
        return {};
    }
    s.ready = false;
    const vk::Extent2D input{color.info.size.width, color.info.size.height};
    s.input = input;
    const auto output = OutputSize(input);
    if (!rec.dibr || rec.menu || !rec.has_render_position || !rec.has_projection ||
        !(rec.near_z > 0 && rec.far_z > rec.near_z) ||
        depth.info.size.width != input.width || depth.info.size.height != input.height ||
        color.backing->num_samples != 1 || depth.backing->num_samples != 1) {
        s.previous_facts.valid = false;
        return {};
    }
    VrTemporalPair::Plan plan{};
    plan.views = 1;
    plan.eyes[0] = {input.width, input.height, output.width, output.height};
    plan.provider_flags = ProviderFlags(config);
    if (config.upscaler == VR::Graphics::Upscaler::Dlss)
        plan.dlss_preset = DlssPresetForFrame(config, s.frame); // a model change recreates DLSS
    const auto begin = s.runtime.Begin(config, plan);
    if (begin != VrTemporalPair::Result::Ready) {
        s.previous_facts.valid = false;
        const bool failed = begin == VrTemporalPair::Result::Failed ||
                            begin == VrTemporalPair::Result::Unavailable ||
                            begin == VrTemporalPair::Result::Invalid;
        // Preparing: no jitter until the provider can resolve (the frame keeps its AA).
        VR::SetUpscalerJitter(0);
        VR::Graphics::PublishStatus(config, failed ? VR::Graphics::RunState::Unavailable
                                                   : VR::Graphics::RunState::Preparing);
        if (failed && !s.failed_logged) {
            s.failed_logged = true;
            LOG_WARNING(Render_Vulkan, "VR DIBR upscale: {} unavailable ({}); the centre keeps "
                        "its {}x{} render and the saved AA", ProviderName(config),
                        s.runtime.Problem().empty() ? "provider refused the plan" : s.runtime.Problem(),
                        input.width, input.height);
        }
        return {};
    }
    s.failed_logged = false;
    s.ready = true;
    // The provider's cycle for the next frames (this one was jittered by the last call's).
    const float ratio = float(input.width) / float(output.width);
    VR::SetUpscalerJitter(VR::RenderTaa::UpscalerPhases(ratio));
    // The first ready frame may already have had its FXAA/SMAA/TAA: it stays as drawn; the AA
    // hook skips them from the next frame on (Ready).
    if (already_resolved) return {};
    s.scheduler.EndRendering();
    color.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    depth.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    VideoCore::ImageViewInfo cv{}, dv{};
    // X's stored bytes (its own UNORM format, not an sRGB view): DLSS and FSR get the
    // tonemapped frame as S1 gives it to them, and their output keeps that encoding.
    cv.format = color.backing->image.image_ci.format;
    cv.mapping = components;
    dv.format = vk::Format::eR32Sfloat; // the view swaps to the depth format and aspect
    const bool dlss = config.upscaler == VR::Graphics::Upscaler::Dlss;
    std::array<VrTemporalImages::Source, 2> sources{};
    sources[0] = {*color.FindView(cv, false).image_view, *depth.FindView(dv, false).image_view,
                  input.width, input.height, color.GetImage(), depth.GetImage(),
                  color.backing->image.image_ci.format, depth.backing->image.image_ci.format, 0};
    // Bit 46: plain DLSS reads X and the depth in place (no canonical copies).
    const bool direct = dlss && FastRender::On(DlssDirect) && DlssDirectEligible(dlss, false);
    if (!s.images.Prepare(config, true, plan, sources, direct, 0)) {
        s.ready = false;
        s.previous_facts.valid = false;
        VR::Graphics::PublishStatus(config, VR::Graphics::RunState::Retrying);
        return {};
    }
    namespace RT = VR::RenderTaa;
    const auto rows = RT::CameraRows(rec.base_yaw - rec.head_yaw, -rec.head_pitch, rec.head_roll);
    const std::array<float, 3> pos{rec.render_position.x, rec.render_position.y, rec.render_position.z};
    VrMotionCamera::Camera camera{};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) camera.view[4 * r + c] = rows[3 * r + c];
        for (unsigned c = 0; c < 3; ++c) camera.view[4 * r + 3] -= rows[3 * r + c] * pos[c];
    }
    const auto inverse = VrMotionCamera::Inverse(camera.view);
    if (!inverse) {
        s.ready = false;
        s.previous_facts.valid = false;
        return {};
    }
    camera.inverse = *inverse;
    const float cot = 1.f / std::tan(rec.proj_vfov * .5f);
    const float aspect = rec.proj_aspect > 0 ? rec.proj_aspect : float(input.width) / input.height;
    camera.projection = {xs * cot / aspect, ys * cot, rec.far_z / (rec.far_z - rec.near_z),
                         -rec.near_z * rec.far_z / (rec.far_z - rec.near_z)};
    RT::FrameFacts facts{};
    facts.valid = true;
    facts.width = input.width;
    facts.height = input.height;
    facts.turn_serial = rec.turn_serial;
    facts.space_serial = rec.space_serial;
    facts.world_scale = rec.world_scale;
    facts.time_s = double(rec.tick_ns) * 1e-9;
    facts.rows = rows;
    facts.pos = pos;
    const bool reset = RT::ResetReason(s.previous_facts, facts) != RT::Reset::None;
    // This frame's jitter (FrameTaaJitter, the one its draws were shifted by), in input pixels:
    // the viewport shift (x right, y down) as native stereo gives it; a frame still jittered by
    // aa_mode=taa's epilogue (the first one) in its NDC direction.
    const auto jitter = VR::FrameTaaJitter();
    const std::array<float, 2> j{
        !jitter.on ? 0.f : jitter.viewport ? jitter.px_x : jitter.px_x * xs,
        !jitter.on ? 0.f : jitter.viewport ? jitter.px_y : jitter.px_y * ys};
    auto frames = s.images.Images();
    const auto& f = frames[0];
    // The characters' and the player's own vectors where their motion variants drew this frame
    // (the compose pass keeps camera motion elsewhere, and where a later draw covered them).
    const vk::ImageView objects =
        s.objects_active ? s.objects.PrepareRead(input.width, input.height)[0] : vk::ImageView{};
    const bool have_objects = bool(objects) && !reset;
    std::array<VrCameraMotion::Eye, 2> motion{};
    motion[0] = {vk::ImageView(f.depth.view), have_objects ? objects : vk::ImageView{},
                 vk::ImageView(f.motion.view), vk::Image(f.motion.image),
                 VrMotionCamera::Build(camera, reset ? camera : s.previous, input.width,
                                       input.height, j, reset ? j : s.previous_jitter,
                                       have_objects)};
    if (!s.motion.Record(config, true, motion, nullptr, 1)) {
        s.ready = false;
        s.previous_facts.valid = false;
        return {};
    }
    frames[0].jitter_x = j[0];
    frames[0].jitter_y = j[1];
    frames[0].reset = reset;
    frames[0].frame_ms = reset ? 11.111f : float((facts.time_s - s.previous_facts.time_s) * 1000.);
    if (!(frames[0].frame_ms > 0) || frames[0].frame_ms > 1000.f) frames[0].frame_ms = 11.111f;
    VrTemporalPair::Frame frame{};
    frame.eyes = frames;
    frame.direct_inputs = s.images.Direct();
    // FSR's camera: the centre's planes and vertical field of view.
    frame.near_plane = rec.near_z;
    frame.far_plane = rec.far_z;
    frame.vertical_fov = {rec.proj_vfov, rec.proj_vfov};
    const auto resolved = s.runtime.Resolve(++s.frame, frame);
    if (resolved != VrTemporalPair::Result::Ready) {
        s.ready = false;
        s.previous_facts.valid = false;
        const bool failed = resolved == VrTemporalPair::Result::Failed ||
                            resolved == VrTemporalPair::Result::Unavailable;
        VR::Graphics::PublishStatus(config, failed ? VR::Graphics::RunState::Unavailable
                                                   : VR::Graphics::RunState::Retrying);
        if (failed) VR::SetUpscalerJitter(0);
        if (++s.refused <= 8 || s.refused % 300 == 0)
            LOG_WARNING(Render_Vulkan, "VR DIBR upscale: frame {} not reconstructed ({}), {} so far",
                        s.frame, s.runtime.Problem().empty() ? "provider refused the frame"
                                                             : s.runtime.Problem(), s.refused);
        return {};
    }
    if (!s.Store(vk::Image(frames[0].output.image), vk::Format(frames[0].output.format), output)) {
        s.ready = false;
        s.previous_facts.valid = false;
        return {};
    }
    const Output out{s.scene.image, output, vk::ImageLayout::eTransferSrcOptimal};
    s.previous = camera;
    s.previous_jitter = j;
    s.previous_facts = facts;
    VR::NoteTaaResolve(input.width, input.height, true);
    VR::Graphics::PublishStatus(config, VR::Graphics::RunState::Active, {input.width, input.height},
                                {output.width, output.height});
    if (!s.logged || ++s.resolved % 600 == 0) {
        s.logged = true;
        LOG_INFO(Render_Vulkan, "VR DIBR upscale: {} {}x{} -> {}x{} ({}), jitter {} ({} phases, {}), "
                 "{} inputs, {} output, object motion {} (draws {}/{}, characters {}, player {}, "
                 "refused {}), resolved={} skipped={}",
                 ProviderName(config), input.width, input.height, output.width, output.height,
                 input == output ? "native AA" : "super resolution", jitter.on,
                 RT::UpscalerPhases(ratio), jitter.viewport ? "viewport" : "epilogue",
                 frame.direct_inputs ? "direct" : "copied",
                 frames[0].output.format == VK_FORMAT_R8G8B8A8_UNORM ? "8-bit" : "16F+blit",
                 have_objects, draws.motion_draws, draws.draws, draws.characters, draws.player,
                 draws.refused, s.resolved, s.skipped);
    }
    return out;
}
} // namespace Vulkan
