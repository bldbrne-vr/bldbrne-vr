// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_depth_rescale.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/vr_depth_copy_frag.h"
#include "video_core/host_shaders/vr_depth_copy_vert.h"
#include "video_core/host_shaders/vr_depth_rescale_comp.h"
#include "video_core/host_shaders/vr_stencil_copy_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
namespace {
struct Push {
    uint32_t src_size[2];
    uint32_t dst_size[2];
    uint32_t layers;
    uint32_t bytes;
    uint32_t src_offset;
    uint32_t dst_offset;
    uint32_t words;
    uint32_t stride;
};
struct DrawPush {
    uint32_t src_size[2];
    float scale[2];
    uint32_t layer;
};
// Bytes per texel of one aspect in a buffer copy (0: not a depth/stencil aspect of the format).
uint32_t AspectBytes(vk::Format format, vk::ImageAspectFlagBits aspect) {
    if (aspect == vk::ImageAspectFlagBits::eStencil) {
        switch (format) {
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return 1;
        default:
            return 0;
        }
    }
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD16UnormS8Uint:
        return 2;
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return 4;
    default:
        return 0;
    }
}
constexpr vk::DeviceSize Words(vk::DeviceSize bytes) {
    return (bytes + 3) / 4;
}
// The optimal-tiling features of a format, asked of the driver once (one device per process):
// Choose and Blits run for every HUD copy and every frame's items.
vk::FormatFeatureFlags OptimalFeatures(const Instance& instance, vk::Format format) {
    static std::array<std::atomic<uint64_t>, 256> known{}; // bit 32: asked
    const auto index = static_cast<uint32_t>(format);
    if (index >= known.size())
        return instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    uint64_t value = known[index].load(std::memory_order_relaxed);
    if (!(value >> 32)) {
        const auto features =
            instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
        value = (uint64_t{1} << 32) | static_cast<VkFormatFeatureFlags>(features);
        known[index].store(value, std::memory_order_relaxed);
    }
    return vk::FormatFeatureFlags(static_cast<VkFormatFeatureFlags>(value));
}
// An aspect whose draw could not be made (layout, shader or pipeline failure) is never chosen
// again: Choose gives the buffer copy from the next copy on.
std::atomic<bool> g_no_depth_draw{false}, g_no_stencil_draw{false};
} // namespace

VrDepthRescale::VrDepthRescale(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
VrDepthRescale::~VrDepthRescale() {
    if (layout || pipeline || descriptors || src_buffer || dst_buffer || draw_layout ||
        draw_descriptors || !draw_pipelines.empty())
        scheduler.DeferOperation([d = std::move(descriptors), l = std::move(layout),
                                  p = std::move(pipeline), a = std::move(src_buffer),
                                  b = std::move(dst_buffer), dd = std::move(draw_descriptors),
                                  dl = std::move(draw_layout),
                                  dp = std::move(draw_pipelines)]() mutable {
            p.reset();
            l.reset();
            d.reset();
            a.reset();
            b.reset();
            dp.clear();
            dl.reset();
            dd.reset();
        });
}
bool VrDepthRescale::Blits(const Instance& instance, vk::Format format) {
    static const bool never = [] {
        const char* value = std::getenv("VR_TEST_NO_DEPTH_BLIT");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (never)
        return false;
    const auto features = OptimalFeatures(instance, format);
    return (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
           (features & vk::FormatFeatureFlagBits::eBlitDst);
}
bool VrDepthRescale::Scales(vk::Format format) {
    return AspectBytes(format, vk::ImageAspectFlagBits::eDepth) ||
           AspectBytes(format, vk::ImageAspectFlagBits::eStencil);
}
VrDepthRescale::Mode VrDepthRescale::Choose(const Instance& instance, const VideoCore::Image& src,
                                            vk::ImageAspectFlags aspects) {
    const vk::Format format = src.backing->image.image_ci.format;
    if (Blits(instance, format))
        return Mode::Blit;
    static const bool buffer_only = [] {
        const char* value = std::getenv("VR_TEST_DEPTH_COPY");
        return value && std::strcmp(value, "buffer") == 0;
    }();
    const auto features = OptimalFeatures(instance, format);
    const bool depth = !(aspects & vk::ImageAspectFlagBits::eDepth) ||
                       !g_no_depth_draw.load(std::memory_order_relaxed);
    const bool stencil = !(aspects & vk::ImageAspectFlagBits::eStencil) ||
                         (instance.IsShaderStencilExportSupported() &&
                          !g_no_stencil_draw.load(std::memory_order_relaxed));
    if (!buffer_only && depth && stencil && (src.usage_flags & vk::ImageUsageFlagBits::eSampled) &&
        src.backing->num_samples == 1 &&
        (features & vk::FormatFeatureFlagBits::eDepthStencilAttachment) &&
        (features & vk::FormatFeatureFlagBits::eSampledImage))
        return Mode::Draw;
    return Scales(format) ? Mode::Buffer : Mode::None;
}
const VrDepthRescale::DrawPipelines* VrDepthRescale::DrawPipelinesFor(vk::Format format) {
    for (const auto& p : draw_pipelines)
        if (p.format == format)
            return &p;
    if (draw_failed)
        return nullptr;
    const auto device = instance.GetDevice();
    if (!draw_layout) {
        const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eSampledImage, 1,
                                                     vk::ShaderStageFlagBits::eFragment};
        auto d = device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = 1,
             .pBindings = &binding});
        if (d.result != vk::Result::eSuccess) {
            draw_failed = true;
            g_no_depth_draw = g_no_stencil_draw = true;
            return nullptr;
        }
        draw_descriptors = std::move(d.value);
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(DrawPush)};
        auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                    .pSetLayouts = &*draw_descriptors,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push});
        if (l.result != vk::Result::eSuccess) {
            draw_failed = true;
            g_no_depth_draw = g_no_stencil_draw = true;
            return nullptr;
        }
        draw_layout = std::move(l.value);
    }
    const bool has_depth = AspectBytes(format, vk::ImageAspectFlagBits::eDepth) != 0;
    const bool has_stencil = AspectBytes(format, vk::ImageAspectFlagBits::eStencil) != 0;
    const auto vert =
        Compile(HostShaders::VR_DEPTH_COPY_VERT, vk::ShaderStageFlagBits::eVertex, device);
    const auto depth_frag =
        has_depth
            ? Compile(HostShaders::VR_DEPTH_COPY_FRAG, vk::ShaderStageFlagBits::eFragment, device)
            : vk::ShaderModule{};
    const auto stencil_frag =
        has_stencil && instance.IsShaderStencilExportSupported()
            ? Compile(HostShaders::VR_STENCIL_COPY_FRAG, vk::ShaderStageFlagBits::eFragment, device)
            : vk::ShaderModule{};
    // The depth pass writes depth only, the stencil pass (its value exported) stencil only.
    const auto make = [&](vk::ShaderModule frag, bool stencil) -> vk::UniquePipeline {
        if (!vert || !frag)
            return {};
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
            {.stage = vk::ShaderStageFlagBits::eVertex, .module = vert, .pName = "main"},
            {.stage = vk::ShaderStageFlagBits::eFragment, .module = frag, .pName = "main"},
        }};
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{
            .topology = vk::PrimitiveTopology::eTriangleList,
        };
        const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo raster{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .lineWidth = 1.0f,
        };
        const vk::PipelineMultisampleStateCreateInfo multisample{
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
        };
        const vk::StencilOpState replace{
            .failOp = vk::StencilOp::eKeep,
            .passOp = vk::StencilOp::eReplace,
            .depthFailOp = vk::StencilOp::eReplace,
            .compareOp = vk::CompareOp::eAlways,
            .compareMask = 0xff,
            .writeMask = 0xff,
        };
        const vk::PipelineDepthStencilStateCreateInfo depth_state{
            .depthTestEnable = !stencil,
            .depthWriteEnable = !stencil,
            .depthCompareOp = vk::CompareOp::eAlways,
            .stencilTestEnable = stencil,
            .front = replace,
            .back = replace,
        };
        const vk::PipelineColorBlendStateCreateInfo blend{};
        const std::array<vk::DynamicState, 2> dynamic_states{vk::DynamicState::eViewport,
                                                             vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic{
            .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const vk::PipelineRenderingCreateInfo rendering{
            .depthAttachmentFormat = has_depth ? format : vk::Format::eUndefined,
            .stencilAttachmentFormat = has_stencil ? format : vk::Format::eUndefined,
        };
        const vk::GraphicsPipelineCreateInfo ci{
            .pNext = &rendering,
            .stageCount = static_cast<uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_state,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = *draw_layout,
        };
        auto result = device.createGraphicsPipelineUnique({}, ci);
        return result.result == vk::Result::eSuccess ? std::move(result.value)
                                                     : vk::UniquePipeline{};
    };
    DrawPipelines pipes{.format = format};
    pipes.depth = make(depth_frag, false);
    pipes.stencil = make(stencil_frag, true);
    for (const auto module : {vert, depth_frag, stencil_frag})
        if (module)
            device.destroyShaderModule(module);
    // An aspect of the format without its pipeline: that aspect goes by the buffer copy.
    if (has_depth && !pipes.depth) {
        g_no_depth_draw = true;
        LOG_ERROR(Render_Vulkan, "VR depth rescale: no depth draw for {}; buffer copies instead",
                  vk::to_string(format));
    }
    if (has_stencil && instance.IsShaderStencilExportSupported() && !pipes.stencil) {
        g_no_stencil_draw = true;
        LOG_ERROR(Render_Vulkan, "VR depth rescale: no stencil draw for {}; buffer copies instead",
                  vk::to_string(format));
    }
    LOG_INFO(Render_Vulkan, "VR depth rescale: draw pipelines for {} (depth {}, stencil {})",
             vk::to_string(format), bool(pipes.depth), bool(pipes.stencil));
    draw_pipelines.push_back(std::move(pipes));
    return &draw_pipelines.back();
}
bool VrDepthRescale::Draw(const RecorderProxy& cmd, VideoCore::Image& src, vk::Extent2D src_size,
                          vk::Image dst, vk::Extent2D dst_size, uint32_t layers,
                          vk::Format format, vk::ImageAspectFlags aspects,
                          std::span<vk::UniqueImageView> targets, vk::ImageAspectFlags keep) {
    if (!layers || targets.size() < layers || !src_size.width || !src_size.height ||
        !dst_size.width || !dst_size.height)
        return false;
    const auto* pipes = DrawPipelinesFor(format);
    if (!pipes)
        return false;
    const bool depth = bool(aspects & vk::ImageAspectFlagBits::eDepth);
    const bool stencil = bool(aspects & vk::ImageAspectFlagBits::eStencil);
    if ((depth && !pipes->depth) || (stencil && !pipes->stencil))
        return false; // that aspect's flag is set: the next copy is a buffer copy
    // The source's aspects as sampled array views, kept by the texture cache with the image: a
    // native stereo image takes layer-0 views only and makes their two-layer array itself.
    if (src.native_stereo ? layers > 2 : layers > src.info.resources.layers)
        return false;
    VideoCore::ImageViewInfo info{};
    info.type = src.native_stereo ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DArray;
    info.range.extent.layers = src.native_stereo ? 1 : layers;
    const auto array_view = [&](vk::Format view_format) -> vk::ImageView {
        info.format = view_format;
        const auto& view = src.FindView(info, false);
        return src.native_stereo ? *view.native_array : *view.image_view;
    };
    vk::ImageView depth_view{}, stencil_view{};
    if (depth) {
        depth_view = array_view(AspectBytes(format, vk::ImageAspectFlagBits::eDepth) == 2
                                    ? vk::Format::eR16Unorm
                                    : vk::Format::eR32Sfloat);
    }
    if (stencil) {
        stencil_view = array_view(vk::Format::eR8Uint);
    }
    if ((depth && !depth_view) || (stencil && !stencil_view))
        return false;
    const bool has_depth = AspectBytes(format, vk::ImageAspectFlagBits::eDepth) != 0;
    const bool has_stencil = AspectBytes(format, vk::ImageAspectFlagBits::eStencil) != 0;
    const vk::ImageAspectFlags target_aspects =
        (has_depth ? vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth} : vk::ImageAspectFlags{}) |
        (has_stencil ? vk::ImageAspectFlags{vk::ImageAspectFlagBits::eStencil} : vk::ImageAspectFlags{});
    const auto device = instance.GetDevice();
    for (uint32_t layer = 0; layer < layers; ++layer) {
        if (targets[layer])
            continue;
        auto view = device.createImageViewUnique(
            {.image = dst,
             .viewType = vk::ImageViewType::e2D,
             .format = format,
             .subresourceRange = {target_aspects, 0, 1, layer, 1}});
        if (view.result != vk::Result::eSuccess)
            return false;
        targets[layer] = std::move(view.value);
    }
    const auto pass = [&](vk::Pipeline pipe, vk::ImageView view, const DrawPush& push) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe);
        const vk::DescriptorImageInfo image{.imageView = view,
                                            .imageLayout =
                                                vk::ImageLayout::eDepthStencilReadOnlyOptimal};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eSampledImage,
                                           .pImageInfo = &image};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *draw_layout, 0, write);
        cmd.pushConstants(*draw_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push),
                          &push);
        cmd.draw(3, 1, 0, 0);
    };
    for (uint32_t layer = 0; layer < layers; ++layer) {
        // An aspect drawn is overwritten everywhere (stored, never loaded); one kept is loaded and
        // stored; any other is neither (its contents are not valid: no traffic for it).
        const auto attachment = [&](bool drawn, vk::ImageAspectFlagBits aspect) {
            const bool kept = !drawn && (keep & aspect);
            return vk::RenderingAttachmentInfo{
                .imageView = *targets[layer],
                .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .loadOp = kept ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eDontCare,
                .storeOp = drawn || kept ? vk::AttachmentStoreOp::eStore
                                         : vk::AttachmentStoreOp::eDontCare,
            };
        };
        const auto depth_attachment = attachment(depth, vk::ImageAspectFlagBits::eDepth);
        const auto stencil_attachment = attachment(stencil, vk::ImageAspectFlagBits::eStencil);
        cmd.beginRendering(vk::RenderingInfo{
            .renderArea = {{0, 0}, dst_size},
            .layerCount = 1,
            .pDepthAttachment = has_depth ? &depth_attachment : nullptr,
            .pStencilAttachment = has_stencil ? &stencil_attachment : nullptr,
        });
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(dst_size.width),
                                        static_cast<float>(dst_size.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{{0, 0}, dst_size});
        const DrawPush push{.src_size = {src_size.width, src_size.height},
                            .scale = {static_cast<float>(src_size.width) / dst_size.width,
                                      static_cast<float>(src_size.height) / dst_size.height},
                            .layer = layer};
        if (depth)
            pass(*pipes->depth, depth_view, push);
        if (stencil)
            pass(*pipes->stencil, stencil_view, push);
        cmd.endRendering();
    }
    // Our pipeline, push constants and viewport replaced the game's: its next draw sets them.
    ScratchPush::Invalidate();
    scheduler.GetDynamicState().Invalidate();
    return true;
}
bool VrDepthRescale::Init() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
        {{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
         {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = 2,
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess)
        return false;
    descriptors = std::move(d.value);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                .pSetLayouts = &*descriptors,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push});
    if (l.result != vk::Result::eSuccess)
        return false;
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_DEPTH_RESCALE_COMP, vk::ShaderStageFlagBits::eCompute, device);
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
    return true;
}
// Grown only, never shrunk: the sizes follow the eye size, which a session rarely changes.
bool VrDepthRescale::Buffers(vk::DeviceSize src_bytes, vk::DeviceSize dst_bytes) {
    const auto grow = [&](std::unique_ptr<VideoCore::UniqueBuffer>& buffer,
                          vk::DeviceSize& capacity, vk::DeviceSize bytes,
                          vk::BufferUsageFlags usage) {
        if (buffer && capacity >= bytes)
            return;
        if (buffer)
            scheduler.DeferOperation([old = std::move(buffer)]() mutable { old.reset(); });
        buffer = std::make_unique<VideoCore::UniqueBuffer>(instance.GetDevice(),
                                                           instance.GetAllocator());
        buffer->Create({.size = bytes, .usage = usage | vk::BufferUsageFlagBits::eStorageBuffer},
                       VideoCore::MemoryUsage::DeviceLocal, nullptr);
        capacity = bytes;
        LOG_INFO(Render_Vulkan, "VR depth rescale: {} buffer {} MiB",
                 &buffer == &src_buffer ? "source" : "destination", bytes >> 20);
    };
    grow(src_buffer, src_capacity, src_bytes, vk::BufferUsageFlagBits::eTransferDst);
    grow(dst_buffer, dst_capacity, dst_bytes, vk::BufferUsageFlagBits::eTransferSrc);
    return src_buffer && dst_buffer;
}
bool VrDepthRescale::Record(const RecorderProxy& cmd, vk::Image src, vk::Extent2D src_size,
                            vk::Image dst, vk::Extent2D dst_size, uint32_t layers,
                            vk::Format format, vk::ImageAspectFlags aspects) {
    if (failed || !layers || !src_size.width || !src_size.height || !dst_size.width ||
        !dst_size.height)
        return false;
    // Depth first, then stencil, each at a word-aligned offset (a depth/stencil buffer copy
    // needs a multiple of 4) in both buffers.
    struct Part {
        vk::ImageAspectFlagBits aspect;
        uint32_t bytes;
        vk::DeviceSize src_offset, dst_offset;
    };
    std::array<Part, 2> parts{};
    uint32_t count = 0;
    vk::DeviceSize src_bytes = 0, dst_bytes = 0;
    for (const auto aspect : {vk::ImageAspectFlagBits::eDepth, vk::ImageAspectFlagBits::eStencil}) {
        if (!(aspects & aspect))
            continue;
        const uint32_t bytes = AspectBytes(format, aspect);
        if (!bytes)
            return false;
        parts[count++] = {aspect, bytes, src_bytes, dst_bytes};
        src_bytes += Words(vk::DeviceSize(src_size.width) * src_size.height * layers * bytes) * 4;
        dst_bytes += Words(vk::DeviceSize(dst_size.width) * dst_size.height * layers * bytes) * 4;
    }
    if (!count || dst_bytes / 4 > 0xffffffffull)
        return false;
    if (!pipeline && !Init()) {
        failed = true;
        LOG_ERROR(Render_Vulkan, "VR depth rescale: pipeline initialization failed");
        return false;
    }
    if (!Buffers(src_bytes, dst_bytes))
        return false;
    const vk::Buffer in = *src_buffer, out = *dst_buffer;
    const auto memory_barrier = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                    vk::PipelineStageFlags2 dst_stage,
                                    vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 barrier{.srcStageMask = src_stage,
                                         .srcAccessMask = src_access,
                                         .dstStageMask = dst_stage,
                                         .dstAccessMask = dst_access};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    };
    // The previous rescale's reads and writes of both buffers come before this one's writes.
    memory_barrier(vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
                   vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderWrite,
                   vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderWrite);
    for (uint32_t i = 0; i < count; ++i) {
        const vk::BufferImageCopy copy{
            .bufferOffset = parts[i].src_offset,
            .imageSubresource = {parts[i].aspect, 0, 0, layers},
            .imageExtent = {src_size.width, src_size.height, 1}};
        cmd.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, in, copy);
    }
    memory_barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                   vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const vk::DescriptorBufferInfo infos[2] = {{in, 0, VK_WHOLE_SIZE}, {out, 0, VK_WHOLE_SIZE}};
    const std::array<vk::WriteDescriptorSet, 2> writes{
        {{.dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = vk::DescriptorType::eStorageBuffer,
          .pBufferInfo = &infos[0]},
         {.dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = vk::DescriptorType::eStorageBuffer,
          .pBufferInfo = &infos[1]}}};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    ScratchPush::Invalidate();
    constexpr uint32_t GroupsPerRow = 32768;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t per_word = 4 / parts[i].bytes;
        const uint64_t texels = uint64_t(dst_size.width) * dst_size.height * layers;
        const uint32_t words = uint32_t((texels + per_word - 1) / per_word);
        const uint32_t groups = (words + 63) / 64;
        const uint32_t columns = std::min(groups, GroupsPerRow);
        const Push push{.src_size = {src_size.width, src_size.height},
                        .dst_size = {dst_size.width, dst_size.height},
                        .layers = layers,
                        .bytes = parts[i].bytes,
                        .src_offset = uint32_t(parts[i].src_offset / 4),
                        .dst_offset = uint32_t(parts[i].dst_offset / 4),
                        .words = words,
                        .stride = columns * 64};
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch(columns, (groups + columns - 1) / columns, 1);
    }
    memory_barrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                   vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    for (uint32_t i = 0; i < count; ++i) {
        const vk::BufferImageCopy copy{
            .bufferOffset = parts[i].dst_offset,
            .imageSubresource = {parts[i].aspect, 0, 0, layers},
            .imageExtent = {dst_size.width, dst_size.height, 1}};
        cmd.copyBufferToImage(out, dst, vk::ImageLayout::eTransferDstOptimal, copy);
    }
    return true;
}
} // namespace Vulkan
