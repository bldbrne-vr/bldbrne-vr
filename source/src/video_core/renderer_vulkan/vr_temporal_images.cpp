// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "video_core/renderer_vulkan/vr_temporal_profile.h"
#include "video_core/renderer_vulkan/vr_temporal_images.h"
#include "video_core/host_shaders/vr_temporal_prepare_comp.h"
#include "video_core/host_shaders/vr_temporal_merge_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include "video_core/renderer_vulkan/vr_temporal_optimizations.h"
#include "video_core/renderer_vulkan/vr_fsr4_interleave.h"
#include "video_core/renderer_vulkan/vr_dlss_direct.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
#include "video_core/buffer_cache/buffer.h"
#include "common/logging/log.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
// Bit 37: these passes go through the recorder like bbport's motion pass; off, they borrow
// the raw command buffer (raw() keeps each call site's flush label).
template <class Raw, class Body>
void RecordPass(Scheduler& scheduler, Raw&& raw, Body&& body) {
    if (UpscalerFastOn(UpscalerFastPart::Recording)) {
        body(scheduler.Commands());
        UpscalerFastNote(g_upscaler_fast.raw_avoided);
    } else {
        body(raw());
    }
}
float HalfToFloat(uint16_t h) {
    const float sign = (h & 0x8000u) ? -1.0f : 1.0f;
    const int exponent = (h >> 10) & 0x1f;
    const int mantissa = h & 0x3ff;
    if (exponent == 0)
        return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31)
        return mantissa ? std::numeric_limits<float>::quiet_NaN()
                        : sign * std::numeric_limits<float>::infinity();
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}
} // namespace
VrTemporalImages::VrTemporalImages(const Instance &i, Scheduler &s) : instance(i), scheduler(s) {}
VrTemporalImages::~VrTemporalImages() {
    Release();
    if (descriptors || layout || pipeline)
        scheduler.DeferOperation(
            [d = std::move(descriptors), l = std::move(layout), p = std::move(pipeline),
             m = std::move(merge_pipeline)]() mutable {
                m.reset();
                p.reset();
                l.reset();
                d.reset();
            });
}
void VrTemporalImages::ReleaseVerify() {
    verify_pending = false;
    verify_mapped = nullptr;
    if (!verify.image && !twin_motion.image && !verify_download)
        return;
    // A pending readback may still be in flight: all leave with the current tick.
    scheduler.DeferOperation([surface = std::move(verify), motion = std::move(twin_motion),
                              buffer = std::move(verify_download)]() mutable {
        surface.view.reset();
        surface.image.Destroy();
        motion.view.reset();
        motion.image.Destroy();
        buffer.reset();
    });
    Surface empty, empty_motion;
    verify = std::move(empty);
    twin_motion = std::move(empty_motion);
}
void VrTemporalImages::Release() {
    ready = false;
    allocated = {};
    ReleaseVerify();
    bool any = false;
    for (const auto &eye : eyes)
        for (const auto &s : eye)
            any |= bool(s.image);
    if (!any)
        return;
    scheduler.DeferOperation([retired = std::move(eyes)]() mutable {
        for (auto &eye : retired)
            for (auto &s : eye) {
                s.view.reset();
                s.image.Destroy();
            }
    });
    std::array<Eye, 2> empty;
    eyes = std::move(empty);
}
bool VrTemporalImages::Create(Surface &s, vk::Format format, uint32_t width, uint32_t height) {
    const auto device = instance.GetDevice();
    s.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    s.image.image_ci = {.imageType = vk::ImageType::e2D,
                        .format = format,
                        .extent = {width, height, 1},
                        .mipLevels = 1,
                        .arrayLayers = 1,
                        .samples = vk::SampleCountFlagBits::e1,
                        .tiling = vk::ImageTiling::eOptimal,
                        .usage = vk::ImageUsageFlagBits::eStorage |
                                 vk::ImageUsageFlagBits::eSampled |
                                 vk::ImageUsageFlagBits::eTransferSrc |
                                 vk::ImageUsageFlagBits::eTransferDst,
                        .initialLayout = vk::ImageLayout::eUndefined};
    const VmaAllocationCreateInfo memory{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(),
                       reinterpret_cast<const VkImageCreateInfo *>(&s.image.image_ci), &memory,
                       &image, &s.image.allocation, nullptr) != VK_SUCCESS)
        return false;
    s.image.image = image;
    auto view = device.createImageViewUnique({.image = s.image,
                                              .viewType = vk::ImageViewType::e2D,
                                              .format = format,
                                              .subresourceRange = Range});
    if (view.result != vk::Result::eSuccess)
        return false;
    s.view = std::move(view.value);
    s.format = format;
    s.width = width;
    s.height = height;
    return true;
}
bool VrTemporalImages::Allocate(const VrTemporalPair::Plan &plan) {
    if (allocated == plan && eyes[0][0].image)
        return true;
    Release();
    constexpr std::array formats{vk::Format::eR16G16B16A16Sfloat, vk::Format::eR32Sfloat,
                                 vk::Format::eR16G16Sfloat, vk::Format::eR16G16B16A16Sfloat};
    // Capability queries occur only on allocation/reconfiguration, never per draw/frame.
    for (auto format : formats) {
        const auto f =
            instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
        const auto need = vk::FormatFeatureFlagBits::eStorageImage |
                          vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eTransferSrc;
        if ((f & need) != need)
            return false;
    }
    for (uint32_t eye = 0; eye < plan.views; ++eye)
        for (uint32_t kind = 0; kind < 4; ++kind) {
            const auto &s = plan.eyes[eye];
            if (!Create(eyes[eye][kind], formats[kind], kind == 3 ? s.output_width : s.input_width,
                        kind == 3 ? s.output_height : s.input_height)) {
                Release();
                return false;
            }
        }
    // FAST_UPSCALER_VERIFY / FAST_FSR4_INTERLEAVE_VERIFY: the left-eye twin's output, compared
    // with eyes[0]'s. FAST_DLSS_DIRECT_VERIFY: its eye's twin output, and the twin's own
    // motion vectors.
    const bool dlss_twin = (plan.provider_flags & VrTemporalPair::DlssVerifyTwin) != 0;
    const bool fsr_twin = (plan.provider_flags & (VrTemporalPair::Fsr4VerifyTwin |
                                                  VrTemporalPair::Fsr4VerifyInterleave)) != 0;
    const auto &twin = plan.eyes[dlss_twin ? VrTemporalPair::DlssTwinEye(plan.provider_flags) : 0];
    if (((fsr_twin || dlss_twin) &&
         !Create(verify, formats[3], twin.output_width, twin.output_height)) ||
        (dlss_twin && !Create(twin_motion, formats[2], twin.input_width, twin.input_height))) {
        Release();
        return false;
    }
    allocated = plan;
    return true;
}
bool VrTemporalImages::Init() {
    if (!instance.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats)
        return false;
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < 4; ++i)
        bindings[i] = {
            i, i < 2 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eStorageImage, 1,
            vk::ShaderStageFlagBits::eCompute};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = 4,
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess)
        return false;
    descriptors = std::move(d.value);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 20};
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                .pSetLayouts = &*descriptors,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push});
    if (l.result != vk::Result::eSuccess)
        return false;
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_TEMPORAL_PREPARE_COMP, vk::ShaderStageFlagBits::eCompute, device);
    if (!module)
        return false;
    auto p = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
         .layout = *layout});
    device.destroyShaderModule(module);
    if (p.result != vk::Result::eSuccess)
        return false;
    pipeline = std::move(p.value);
    const auto merge_module = Compile(HostShaders::VR_TEMPORAL_MERGE_COMP,
                                      vk::ShaderStageFlagBits::eCompute, device);
    if (!merge_module) return false;
    auto merge = device.createComputePipelineUnique({},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = merge_module,
                   .pName = "main"}, .layout = *layout});
    device.destroyShaderModule(merge_module);
    if (merge.result != vk::Result::eSuccess) return false;
    merge_pipeline = std::move(merge.value);
    return true;
}
bool VrTemporalImages::Merge(const std::array<vk::ImageView, 2>& scene) {
    if (!ready || !merge_pipeline || !scene[0] || (allocated.views == 2 && !scene[1])) return false;
    RecordPass(scheduler, [&] { return scheduler.CommandBuffer(); }, [&](auto cmd) {
    // Includes the provider output and the native scene populated from its reduced proxy.
    const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1, .pMemoryBarriers=&before});
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *merge_pipeline);
    ScratchPush::Invalidate();
    for (uint32_t eye=0; eye<allocated.views; ++eye) {
        // Bit 46 left this eye's canonical colour uncopied: the game's alpha comes from the source.
        // Reduced raster reads the proxy (the copy's own source, same nearest texel). Natively the
        // source is the scene being merged into: the shader reads it in place (imageLoad at the
        // texel it writes, which the copy held), and the stale canonical view is bound unused.
        // A colour FAST_DLSS_DIRECT_LAYER_COPY kept copied reads its canonical copy, as without
        // the bit.
        const bool in_place = direct && ((direct_color >> eye) & 1u);
        const bool from_source = in_place && TemporalOptimizations() &&
                                 direct_sources[eye].color != scene[eye];
        const bool direct_alpha = TemporalOptimizations() && (!in_place || from_source);
        const std::array<vk::DescriptorImageInfo,3> infos{{
            {.imageView=*eyes[eye][3].view, .imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=scene[eye], .imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=from_source ? direct_sources[eye].color : *eyes[eye][0].view,
             .imageLayout=vk::ImageLayout::eGeneral}}};
        const std::array<vk::WriteDescriptorSet,3> writes{{
            {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[0]},
            {.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&infos[1]},
            {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[2]}}};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
        const std::array<uint32_t,5> size{allocated.eyes[eye].output_width, allocated.eyes[eye].output_height,
            allocated.eyes[eye].input_width, allocated.eyes[eye].input_height, uint32_t(direct_alpha)};
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, uint32_t(sizeof(size)), size.data());
        cmd.dispatch((size[0]+7)/8,(size[1]+7)/8,1);
    }
    const vk::MemoryBarrier2 after{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&after});
    });
    return true;
}
bool VrTemporalImages::Prepare(const VR::Graphics::Config &accepted, bool provider_ready,
                               const VrTemporalPair::Plan &plan,
                               const std::array<Source, 2> &sources, bool direct_request,
                               uint32_t copies) {
    ready = false;
    direct = false;
    copied_color = copied_depth = direct_color = direct_depth = 0;
    if (accepted.upscaler == VR::Graphics::Upscaler::Off || !provider_ready) {
        Release();
        return false;
    }
    if (failed)
        return false;
    if (plan.views != 1 && plan.views != 2) return false;
    const uint32_t active_mask = (1u << plan.views) - 1u;
    // Reject both eyes up front; no partial-eye allocation or GPU commands for bad inputs.
    for (uint32_t eye = 0; eye < plan.views; ++eye) {
        const auto &s = plan.eyes[eye];
        const auto &in = sources[eye];
        if (!in.color || !in.depth || !s.input_width || !s.input_height ||
            s.input_width > s.output_width || s.input_height > s.output_height ||
            s.output_width > 16384 || s.output_height > 16384 || in.width != s.input_width ||
            in.height != s.input_height)
            return false;
    }
    if (plan.views == 2 && (sources[0].color == sources[1].color || sources[0].depth == sources[1].depth))
        return false;
    // Bit 46: the provider reads each eye's own layer of the source, only in formats whose
    // values the copy kept exactly (RGBA16F colour, f32 depth aspect); otherwise copy as before.
    if (direct_request) {
        for (uint32_t eye = 0; eye < plan.views; ++eye) {
            const auto &in = sources[eye];
            direct_request = direct_request && in.color_image && in.depth_image &&
                             in.layer == eye &&
                             in.color_format == vk::Format::eR16G16B16A16Sfloat &&
                             (in.depth_format == vk::Format::eD32Sfloat ||
                              in.depth_format == vk::Format::eD32SfloatS8Uint);
        }
    }
    copies &= active_mask;
    // Bit 46: what the provider reads in place. A layer-1 input FAST_DLSS_DIRECT_LAYER_COPY names
    // (default: depth) is copied and read from its canonical image, since NGX reads it from
    // layer 0 (vr_dlss_direct.h); the eye's other input stays in place.
    uint32_t in_place_color = 0, in_place_depth = 0;
    if (direct_request) {
        uint32_t layered = 0;
        for (uint32_t eye = 0; eye < plan.views; ++eye)
            if (sources[eye].layer != 0)
                layered |= 1u << eye;
        const u32 layer_copy = DlssDirectLayerCopy();
        in_place_color = active_mask & ~((layer_copy & DlssDirectColor) ? layered : 0u);
        in_place_depth = active_mask & ~((layer_copy & DlssDirectDepth) ? layered : 0u);
        direct_request = (in_place_color | in_place_depth) != 0;
    }
    // Per eye: copied for the provider (not read in place) or for the verify twin (`copies`).
    const uint32_t copy_color = direct_request ? (copies | (active_mask & ~in_place_color)) : active_mask;
    const uint32_t copy_depth = direct_request ? (copies | (active_mask & ~in_place_depth)) : active_mask;
    if ((!pipeline && !Init()) || !Allocate(plan)) {
        failed = true;
        Release();
        return false;
    }
    scheduler.EndRendering();
    TemporalProfile::Scope prepare_profile(scheduler, TemporalProfile::Prepare,
                                           "Temporal input preparation (both eyes)");
    RecordPass(scheduler, [&] { return scheduler.CommandBuffer(); }, [&](auto cmd) {
    std::array<vk::ImageMemoryBarrier2, 10> barriers{};
    uint32_t n = 0;
    std::array<Surface *, 10> surfaces{};
    for (uint32_t eye = 0; eye < plan.views; ++eye)
        for (auto &s : eyes[eye])
            surfaces[n++] = &s;
    if (verify.image) // FAST_UPSCALER_VERIFY/FAST_DLSS_DIRECT_VERIFY twin output
        surfaces[n++] = &verify;
    if (twin_motion.image) // FAST_DLSS_DIRECT_VERIFY twin motion vectors
        surfaces[n++] = &twin_motion;
    for (uint32_t i = 0; i < n; ++i) {
            auto &s = *surfaces[i];
            barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                             .srcAccessMask = s.initialized ? vk::AccessFlagBits2::eMemoryRead |
                                                                  vk::AccessFlagBits2::eMemoryWrite
                                                            : vk::AccessFlags2{},
                             .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                             vk::PipelineStageFlagBits2::eTransfer,
                             .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                              vk::AccessFlagBits2::eShaderWrite |
                                              vk::AccessFlagBits2::eTransferWrite,
                             .oldLayout = s.initialized ? vk::ImageLayout::eGeneral
                                                        : vk::ImageLayout::eUndefined,
                             .newLayout = vk::ImageLayout::eGeneral,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .image = s.image,
                             .subresourceRange = Range};
            s.initialized = true;
        }
    // Bit 46: the provider reads the scene where the copy used to, so make the scene's writes
    // visible to it from any stage (the copy's own barrier below covered compute reads only).
    // The scene's transition to GENERAL already waits for those writes: no extra drain.
    const vk::MemoryBarrier2 scene{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                   .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                   .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                   .dstAccessMask = vk::AccessFlagBits2::eMemoryRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = direct_request ? 1u : 0u,
                                            .pMemoryBarriers = direct_request ? &scene : nullptr,
                                            .imageMemoryBarrierCount = n,
                                            .pImageMemoryBarriers = barriers.data()});
    if (!(copy_color | copy_depth))
        return;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    ScratchPush::Invalidate();
    for (uint32_t eye = 0; eye < plan.views; ++eye) {
        // Bit 46: an input read in place is not copied (1 colour, 2 depth; 3 without the bit).
        const uint32_t mask = ((copy_color >> eye) & 1u) | (((copy_depth >> eye) & 1u) << 1);
        if (!mask)
            continue;
        const std::array<vk::ImageView, 4> views{sources[eye].color, sources[eye].depth,
                                                 *eyes[eye][0].view, *eyes[eye][1].view};
        std::array<vk::DescriptorImageInfo, 4> infos{};
        std::array<vk::WriteDescriptorSet, 4> writes{};
        for (uint32_t i = 0; i < 4; ++i) {
            infos[i] = {.imageView = views[i], .imageLayout = vk::ImageLayout::eGeneral};
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i < 2 ? vk::DescriptorType::eSampledImage
                                                 : vk::DescriptorType::eStorageImage,
                         .pImageInfo = &infos[i]};
        }
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
        const std::array<uint32_t, 3> size{sources[eye].width, sources[eye].height, mask};
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0,
                          uint32_t(sizeof(size)), size.data());
        cmd.dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    }
    const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                     .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                     .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    direct = direct_request;
    if (direct)
        direct_sources = sources;
    copied_color = copy_color;
    copied_depth = copy_depth;
    direct_color = direct ? in_place_color : 0u;
    direct_depth = direct ? in_place_depth : 0u;
    ready = true;
    return true;
}
BbDlssImage VrTemporalImages::Canonical(const Surface &s) {
    return BbDlssImage{s.image.image, *s.view, VkImageSubresourceRange(Range), VkFormat(s.format),
                       s.width, s.height};
}
BbDlssImage VrTemporalImages::VerifyOutput() const {
    if (!ready || !verify.image)
        return {};
    return Canonical(verify);
}
BbDlssEvaluate VrTemporalImages::TwinInputs(uint32_t eye) const {
    BbDlssEvaluate out{};
    if (!ready || eye > 1 || !twin_motion.image || !(copied_color & copied_depth & (1u << eye)))
        return out;
    out.color = Canonical(eyes[eye][0]);
    out.depth = Canonical(eyes[eye][1]);
    out.motion = Canonical(twin_motion);
    return out;
}
void VrTemporalImages::Verify(uint64_t frame, uint32_t period, bool twin_evaluated,
                              VerifyKind kind, uint32_t eye, bool interleaved) {
    if (!period || !ready || !verify.image || eye > 1)
        return;
    // Compare the previous readback once its frame has finished on the GPU; never wait.
    if (verify_pending && scheduler.IsFree(verify_ticket))
        CompareVerify();
    if (!twin_evaluated) {
        if (kind == VerifyKind::DlssDirect)
            UpscalerFastNote(g_dlss_direct.verify_skipped);
        else if (allocated.provider_flags & VrTemporalPair::Fsr4VerifyInterleave)
            g_fsr4_interleave.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        else
            UpscalerFastNote(g_upscaler_fast.verify_skipped);
        return;
    }
    if (verify_pending || frame % period)
        return;
    const auto &out = eyes[eye][3];
    if (out.width != verify.width || out.height != verify.height)
        return;
    const uint64_t bytes = uint64_t(out.width) * out.height * 8; // RGBA16F
    if (!verify_download) {
        verify_download =
            std::make_unique<VideoCore::UniqueBuffer>(instance.GetDevice(), instance.GetAllocator());
        VmaAllocationInfo allocation{};
        verify_download->Create({.size = bytes * 2, .usage = vk::BufferUsageFlagBits::eTransferDst},
                                VideoCore::MemoryUsage::Download, &allocation, true);
        verify_mapped = allocation.pMappedData;
    }
    if (!verify_mapped)
        return;
    scheduler.EndRendering();
    // After the provider (Resolve's barrier made both outputs readable); before the merge and
    // the next frame's provider writes, which the second barrier orders after these reads.
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    const std::array<vk::Image, 2> images{vk::Image(out.image), vk::Image(verify.image)};
    for (uint32_t i = 0; i < 2; ++i)
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                       .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                       .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                       .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                       .oldLayout = vk::ImageLayout::eGeneral,
                       .newLayout = vk::ImageLayout::eGeneral,
                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .image = images[i],
                       .subresourceRange = Range};
    const vk::Buffer buffer = *verify_download;
    const auto cmd = scheduler.Commands();
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
    for (uint32_t i = 0; i < 2; ++i) {
        const vk::BufferImageCopy region{.bufferOffset = bytes * i,
                                         .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                         .imageExtent = {out.width, out.height, 1}};
        cmd.copyImageToBuffer(images[i], vk::ImageLayout::eGeneral, buffer, region);
    }
    for (auto &b : barriers) {
        b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        b.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
        b.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        b.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    }
    const vk::BufferMemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = buffer,
                                        .offset = 0,
                                        .size = VK_WHOLE_SIZE};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                            .pBufferMemoryBarriers = &host,
                                            .imageMemoryBarrierCount = 2,
                                            .pImageMemoryBarriers = barriers.data()});
    verify_ticket = scheduler.CurrentTick();
    verify_frame = frame;
    verify_interleaved = interleaved;
    verify_kind = kind;
    verify_eye = eye;
    verify_direct = direct;
    verify_pending = true;
}
void VrTemporalImages::CompareVerify() {
    verify_pending = false;
    if (!verify_download || !verify_mapped)
        return;
    vmaInvalidateAllocation(instance.GetAllocator(), verify_download->allocation, 0,
                            VK_WHOLE_SIZE);
    const auto &out = eyes[verify_eye][3];
    const size_t texels = size_t(out.width) * out.height;
    const auto *optimized = static_cast<const uint16_t *>(verify_mapped);
    const auto *original = optimized + texels * 4;
    // Besides exact bits: relative size of the differences (HDR values; the floor keeps
    // near-black texels from dominating), and texels a display could show (> 1/256).
    constexpr float rel_floor = 0.01f, visible_rel = 1.0f / 256.0f;
    uint64_t mismatched = 0, visible = 0;
    float max_abs = 0, max_rel = 0;
    double sum_abs = 0;
    for (size_t i = 0; i < texels; ++i) {
        bool differs = false, shows = false;
        for (size_t c = 0; c < 4; ++c) {
            const uint16_t a = optimized[i * 4 + c], b = original[i * 4 + c];
            if (a == b)
                continue;
            differs = true;
            const float fa = HalfToFloat(a), fb = HalfToFloat(b);
            const float d = std::abs(fa - fb);
            max_abs = std::isnan(d) ? std::numeric_limits<float>::infinity()
                                    : (d > max_abs ? d : max_abs);
            const float rel = d / std::max({std::abs(fa), std::abs(fb), rel_floor});
            max_rel = std::isnan(rel) ? std::numeric_limits<float>::infinity()
                                      : (rel > max_rel ? rel : max_rel);
            shows = shows || !(rel <= visible_rel);
            sum_abs += std::isnan(d) ? 0.0 : double(d);
        }
        mismatched += differs;
        visible += shows;
    }
    // Bit 45's A/A twin (same options, evaluated alone), bit 46's DLSS twin and bit 37's twin
    // (original post pass) share the readback; each keeps its own counters and log line.
    const auto note = [&](auto &stats) {
        constexpr auto relaxed = std::memory_order_relaxed;
        stats.verify_compared.fetch_add(1, relaxed);
        if (mismatched) {
            stats.verify_mismatched_frames.fetch_add(1, relaxed);
            stats.verify_mismatched_texels.fetch_add(mismatched, relaxed);
        }
        if (max_abs > std::bit_cast<float>(stats.verify_max_abs.load(relaxed)))
            stats.verify_max_abs.store(std::bit_cast<uint32_t>(max_abs), relaxed);
    };
    if (verify_kind == VerifyKind::DlssDirect) {
        note(g_dlss_direct);
        if (verify_direct)
            DlssDirectNote(g_dlss_direct.verify_direct);
        LOG_INFO(Render_Vulkan,
                 "FAST dlss_direct verify: frame {} {} eye {}x{} {} vs canonical-copy twin: "
                 "mismatched_texels={} max_abs={:.6g}{}",
                 verify_frame, verify_eye ? "right" : "left", out.width, out.height,
                 verify_direct ? "scene layer views" : "canonical copies (bit off: DLSS vs itself)",
                 mismatched, max_abs, mismatched ? "" : " (bit-exact)");
        return;
    }
    if (allocated.provider_flags & VrTemporalPair::Fsr4VerifyInterleave) {
        note(g_fsr4_interleave);
        LOG_INFO(Render_Vulkan,
                 "FAST fsr4 interleave verify: frame {} left eye {}x{} {} vs twin evaluated alone{}: "
                 "mismatched_texels={} max_abs={:.6g}{}",
                 verify_frame, out.width, out.height,
                 verify_interleaved ? "interleaved" : "per-eye (A/A)",
                 Fsr4InterleaveVerifyReset() ? " (lockstep reset)" : "", mismatched, max_abs,
                 mismatched ? "" : " (bit-exact)");
        return;
    }
    note(g_upscaler_fast);
    const char *what =
        UpscalerFastVerifySame() ? "A/A twin, same FSR post" : "optimized vs original FSR post";
    LOG_INFO(Render_Vulkan,
             "FAST upscaler verify: frame {} left eye {}x{} {}{}: mismatched_texels={} "
             "max_abs={:.6g} max_rel={:.3g} mean_abs={:.3g} visible_texels={}{}",
             verify_frame, out.width, out.height, what,
             UpscalerFastVerifyLockstep() ? " (lockstep reset)" : "", mismatched, max_abs, max_rel,
             sum_abs / double(texels * 4), visible, mismatched ? "" : " (bit-exact)");
}
std::array<BbDlssEvaluate, 2> VrTemporalImages::Images() const {
    std::array<BbDlssEvaluate, 2> out{};
    if (!ready)
        return out;
    for (uint32_t eye = 0; eye < allocated.views; ++eye) {
        out[eye].color = Canonical(eyes[eye][0]);
        out[eye].depth = Canonical(eyes[eye][1]);
        out[eye].motion = Canonical(eyes[eye][2]);
        out[eye].output = Canonical(eyes[eye][3]);
        if (!direct)
            continue;
        // Bit 46: this eye's layer of the scene (or of its reduced proxy), as the caller left it:
        // GENERAL, readable by shaders. The view outlives the frame's GPU work: the capture's
        // views and the proxy's reads are retired through deferred operations.
        const auto &s = direct_sources[eye];
        const auto layer = [&](vk::Image image, vk::ImageView view, vk::ImageAspectFlagBits aspect,
                               vk::Format format) {
            return BbDlssImage{static_cast<VkImage>(image), static_cast<VkImageView>(view),
                               VkImageSubresourceRange{static_cast<VkImageAspectFlags>(aspect), 0,
                                                       1, s.layer, 1},
                               static_cast<VkFormat>(format), s.width, s.height};
        };
        // An input FAST_DLSS_DIRECT_LAYER_COPY keeps copied (right eye's depth by default) is
        // the canonical image set above.
        const bool color_in_place = (direct_color >> eye) & 1u;
        const bool depth_in_place = (direct_depth >> eye) & 1u;
        if (color_in_place)
            out[eye].color = layer(s.color_image, s.color, vk::ImageAspectFlagBits::eColor,
                                   s.color_format);
        if (depth_in_place)
            out[eye].depth = layer(s.depth_image, s.depth, vk::ImageAspectFlagBits::eDepth,
                                   s.depth_format);
        if (!direct_logged[eye]) {
            direct_logged[eye] = true;
            const auto& c = out[eye].color;
            const auto& d = out[eye].depth;
            LOG_INFO(Render_Vulkan,
                     "FAST dlss_direct: eye {} source layer {} | colour {} view={:#x} image={:#x} "
                     "aspect={:#x} layer={}+{} format={} | depth {} view={:#x} image={:#x} "
                     "aspect={:#x} layer={}+{} format={} | layer_copy={}",
                     eye, s.layer, color_in_place ? "in place" : "canonical copy",
                     uint64_t(c.view), uint64_t(c.image), uint32_t(c.range.aspectMask),
                     c.range.baseArrayLayer, c.range.layerCount, int(c.format),
                     depth_in_place ? "in place" : "canonical copy", uint64_t(d.view),
                     uint64_t(d.image), uint32_t(d.range.aspectMask), d.range.baseArrayLayer,
                     d.range.layerCount, int(d.format), DlssDirectLayerCopyName());
        }
    }
    return out;
}
// Diagnostic only: create <VR_TEMPORAL_CAPTURE_DIR>/capture containing 1..8 to
// capture that many consecutive successful frames. Never enabled by normal settings.
void VrTemporalImages::DebugCapture(uint64_t frame, std::string_view metadata) {
    static const char* directory = std::getenv("VR_TEMPORAL_CAPTURE_DIR");
    if (!directory || !ready) return;
    static unsigned remaining{};
    const std::filesystem::path dir(directory);
    std::error_code ec;
    if (!remaining && std::filesystem::exists(dir / "capture", ec)) {
        unsigned requested=2;
        std::ifstream(dir / "capture") >> requested;
        remaining=std::clamp(requested,1u,8u);
        std::filesystem::remove(dir / "capture",ec);
    }
    if (!remaining) return;
    --remaining;
    struct Copy { Surface* surface; uint64_t offset, bytes; unsigned eye, kind; };
    std::array<Copy,8> copies{};
    uint64_t total{};
    unsigned n{};
    for (unsigned eye=0; eye<allocated.views; ++eye) for (unsigned kind=0; kind<4; ++kind) {
        auto& surface=eyes[eye][kind];
        const uint64_t bytes=uint64_t(surface.width)*surface.height*(kind==0 || kind==3 ? 8:4);
        copies[n++]={&surface,total,bytes,eye,kind};
        total=(total+bytes+255)&~uint64_t(255);
    }
    VideoCore::UniqueBuffer download(instance.GetDevice(),instance.GetAllocator());
    VmaAllocationInfo allocation{};
    download.Create({.size=total,.usage=vk::BufferUsageFlagBits::eTransferDst},
                    VideoCore::MemoryUsage::Download,&allocation,true);
    const auto cmd=scheduler.CommandBuffer();
    std::array<vk::ImageMemoryBarrier2,8> barriers{};
    for (unsigned i=0; i<8; ++i) barriers[i]={
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask=vk::AccessFlagBits2::eTransferRead,
        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=copies[i].surface->image,.subresourceRange=Range};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount=8,.pImageMemoryBarriers=barriers.data()});
    for (const auto& c:copies) {
        const vk::BufferImageCopy region{.bufferOffset=c.offset,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageExtent={c.surface->width,c.surface->height,1}};
        cmd.copyImageToBuffer(c.surface->image,vk::ImageLayout::eGeneral,download,region);
    }
    for (auto& b:barriers) {
        b.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
        b.srcAccessMask=vk::AccessFlagBits2::eTransferRead;
        b.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
        b.dstAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount=8,.pImageMemoryBarriers=barriers.data()});
    scheduler.Finish(); // Captures are correctness checks, never performance measurements.
    vmaInvalidateAllocation(instance.GetAllocator(),download.allocation,0,VK_WHOLE_SIZE);
    constexpr std::array names{"color_rgba16f","depth_r32f","motion_rg16f","output_rgba16f"};
    std::filesystem::create_directories(dir,ec);
    const auto prefix=std::to_string(frame);
    for (const auto& c:copies) {
        std::ofstream file(dir/(prefix+"_"+std::to_string(c.eye)+"_"+names[c.kind]+".bin"),std::ios::binary);
        file.write(static_cast<const char*>(allocation.pMappedData)+c.offset,c.bytes);
    }
    std::ofstream(dir/(prefix+".json")) << metadata;
    LOG_INFO(Render_Vulkan,"VR temporal capture: frame {} at {} (diagnostic GPU wait)",frame,directory);
}
} // namespace Vulkan
