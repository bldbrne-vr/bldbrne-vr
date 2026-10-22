// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "video_core/renderer_vulkan/vr_ui_compose.h"
#ifdef ENABLE_OPENXR
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"
#include "video_core/host_shaders/vr_ui_compose_vert.h"
#include "video_core/host_shaders/vr_ui_compose_frag.h"
#include "video_core/native_stereo/push_state_memo.h"

namespace Vulkan {
namespace O = VrOverlay;
namespace {
struct Push {
    std::array<float, 16> mvp;
    std::array<float, 4> uv;
    std::array<float, 4> geometry;
    std::array<float, 4> tint;
};
static_assert(sizeof(Push) == 112);
O::Mat4 Matrix(const XrPosef& p) {
    return O::Mat4::FromPose({{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},
                            {p.position.x,p.position.y,p.position.z}});
}
}
VrUiCompose::~VrUiCompose() { Shutdown(); }
void VrUiCompose::Shutdown() {
    if (!device) return;
    for (auto& slot : slots) {
        if (slot.pending) (void)device.waitForFences(slot.fence, true, UINT64_MAX);
        if (slot.fence) device.destroyFence(slot.fence);
    }
    if (query_pool) device.destroyQueryPool(query_pool);
    query_pool = vk::QueryPool{};
    if (pipeline) device.destroyPipeline(pipeline);
    if (layout) device.destroyPipelineLayout(layout);
    if (set_layout) device.destroyDescriptorSetLayout(set_layout);
    if (sampler) device.destroySampler(sampler);
    if (pool) device.destroyCommandPool(pool);
    slots = {};
    pipeline = vk::Pipeline{};
    layout = vk::PipelineLayout{};
    set_layout = vk::DescriptorSetLayout{};
    sampler = vk::Sampler{};
    pool = vk::CommandPool{};
    device = vk::Device{};
}
bool VrUiCompose::Init(vk::Device dev, u32 family, vk::Format fmt, vk::PhysicalDevice physical) {
    if (pipeline && format == fmt) return true;
    Shutdown(); device = dev; format = fmt;
    const auto fail = [&] { Shutdown(); return false; };
    auto [sr, smp] = device.createSampler({.magFilter=vk::Filter::eLinear,
        .minFilter=vk::Filter::eLinear, .mipmapMode=vk::SamplerMipmapMode::eLinear,
        .addressModeU=vk::SamplerAddressMode::eClampToEdge,
        .addressModeV=vk::SamplerAddressMode::eClampToEdge,
        .addressModeW=vk::SamplerAddressMode::eClampToEdge, .maxLod=16.0f});
    if (sr != vk::Result::eSuccess) return fail();
    sampler = smp;
    const vk::DescriptorSetLayoutBinding binding{.binding=0,
        .descriptorType=vk::DescriptorType::eCombinedImageSampler, .descriptorCount=1,
        .stageFlags=vk::ShaderStageFlagBits::eFragment};
    auto [dr, ds] = device.createDescriptorSetLayout({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=1, .pBindings=&binding});
    if (dr != vk::Result::eSuccess) return fail();
    set_layout = ds;
    const vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eVertex |
        vk::ShaderStageFlagBits::eFragment, .offset=0, .size=sizeof(Push)};
    auto [lr, pl] = device.createPipelineLayout({.setLayoutCount=1, .pSetLayouts=&set_layout,
        .pushConstantRangeCount=1, .pPushConstantRanges=&range});
    if (lr != vk::Result::eSuccess) return fail();
    layout = pl;
    const auto vs = Compile(HostShaders::VR_UI_COMPOSE_VERT, vk::ShaderStageFlagBits::eVertex, device);
    const auto fs = Compile(HostShaders::VR_UI_COMPOSE_FRAG, vk::ShaderStageFlagBits::eFragment, device);
    if (!vs || !fs) {
        if (vs) device.destroyShaderModule(vs);
        if (fs) device.destroyShaderModule(fs);
        return fail();
    }
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex, .module=vs, .pName="main"},
        vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment, .module=fs, .pName="main"}};
    const vk::PipelineVertexInputStateCreateInfo vertex{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount=1, .scissorCount=1};
    const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode=vk::PolygonMode::eFill,
        .cullMode=vk::CullModeFlagBits::eNone, .frontFace=vk::FrontFace::eCounterClockwise, .lineWidth=1};
    const vk::PipelineMultisampleStateCreateInfo msaa{.rasterizationSamples=vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendAttachmentState attachment{.blendEnable=true,
        .srcColorBlendFactor=vk::BlendFactor::eOne, .dstColorBlendFactor=vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp=vk::BlendOp::eAdd, .srcAlphaBlendFactor=vk::BlendFactor::eOne,
        .dstAlphaBlendFactor=vk::BlendFactor::eOneMinusSrcAlpha, .alphaBlendOp=vk::BlendOp::eAdd,
        .colorWriteMask=vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount=1, .pAttachments=&attachment};
    const std::array states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount=2, .pDynamicStates=states.data()};
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount=1, .pColorAttachmentFormats=&format};
    auto [pr, pipe] = device.createGraphicsPipeline({}, {.pNext=&rendering, .stageCount=2,
        .pStages=stages.data(), .pVertexInputState=&vertex, .pInputAssemblyState=&assembly,
        .pViewportState=&viewport, .pRasterizationState=&raster, .pMultisampleState=&msaa,
        .pColorBlendState=&blend, .pDynamicState=&dynamic, .layout=layout});
    device.destroyShaderModule(vs); device.destroyShaderModule(fs);
    if (pr != vk::Result::eSuccess) return fail();
    pipeline = pipe;
    auto [cr, cp] = device.createCommandPool({.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                            .queueFamilyIndex=family});
    if (cr != vk::Result::eSuccess) return fail();
    pool = cp;
    auto [ar, cmds] = device.allocateCommandBuffers({.commandPool=pool,
        .level=vk::CommandBufferLevel::ePrimary, .commandBufferCount=2});
    if (ar != vk::Result::eSuccess) return fail();
    for (u32 i=0; i<2; ++i) {
        slots[i].cmd=cmds[i];
        auto [fr, fence]=device.createFence({});
        if (fr != vk::Result::eSuccess) return fail();
        slots[i].fence=fence;
    }
    const auto families = physical.getQueueFamilyProperties();
    const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
    if (bits > 0) {
        auto [qr, qp] = device.createQueryPool({.queryType=vk::QueryType::eTimestamp, .queryCount=4});
        if (qr == vk::Result::eSuccess) {
            query_pool=qp;
            timestamp_period=physical.getProperties().limits.timestampPeriod;
            timestamp_mask=bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
        }
    }
    return true;
}
vk::CommandBuffer VrUiCompose::Begin() {
    if (!pipeline) return {};
    auto& slot=slots[current];
    if (slot.pending && device.getFenceStatus(slot.fence) != vk::Result::eSuccess) return {};
    if (slot.pending && query_pool) {
        std::array<u64, 2> ticks{};
        if (device.getQueryPoolResults(query_pool,current*2,2,sizeof(ticks),ticks.data(),sizeof(u64),
                                      vk::QueryResultFlagBits::e64)==vk::Result::eSuccess)
            last_gpu_ms=double((ticks[1]-ticks[0])&timestamp_mask)*timestamp_period*1e-6;
    }
    slot.pending=false;
    if (slot.cmd.reset({}) != vk::Result::eSuccess ||
        slot.cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) != vk::Result::eSuccess) return {};
    if (query_pool) {
        slot.cmd.resetQueryPool(query_pool,current*2,2);
        slot.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe,query_pool,current*2);
    }
    return slot.cmd;
}
bool VrUiCompose::Submit(vk::Queue queue, vk::Semaphore signal, u64 value) {
    auto& slot=slots[current];
    if (query_pool) slot.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
                                            query_pool,current*2+1);
    if (slot.cmd.end() != vk::Result::eSuccess || device.resetFences(slot.fence) != vk::Result::eSuccess)
        return false;
    const vk::TimelineSemaphoreSubmitInfo timeline{.signalSemaphoreValueCount=signal ? 1u : 0u,
                                                   .pSignalSemaphoreValues=&value};
    if (queue.submit(vk::SubmitInfo{.pNext=signal ? &timeline : nullptr,
                    .commandBufferCount=1, .pCommandBuffers=&slot.cmd,
                    .signalSemaphoreCount=signal ? 1u : 0u, .pSignalSemaphores=&signal},slot.fence)
        != vk::Result::eSuccess) return false;
    slot.pending=true;
    current ^= 1;
    return true;
}
void VrUiCompose::Draw(vk::CommandBuffer cmd, vk::ImageView target, vk::Extent2D extent,
                       vk::ImageView atlas, vk::Extent2D atlas_extent,
                       std::span<const VrUiGeometry::Piece> pieces, const XrPosef& eye,
                       const XrFovf& fov, const XrPosef& render_head) {
    if (pieces.empty()) return;
    const vk::RenderingAttachmentInfo color{.imageView=target,
        .imageLayout=vk::ImageLayout::eColorAttachmentOptimal, .loadOp=vk::AttachmentLoadOp::eLoad,
        .storeOp=vk::AttachmentStoreOp::eStore};
    cmd.beginRendering({.renderArea={{0,0},extent}, .layerCount=1,
                        .colorAttachmentCount=1, .pColorAttachments=&color});
    cmd.setViewport(0, vk::Viewport{0,0,float(extent.width),float(extent.height),0,1});
    cmd.setScissor(0, vk::Rect2D{{0,0},extent});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,pipeline);
    const vk::DescriptorImageInfo image{.sampler=sampler, .imageView=atlas,
                                        .imageLayout=vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::WriteDescriptorSet write{.dstBinding=0, .descriptorCount=1,
        .descriptorType=vk::DescriptorType::eCombinedImageSampler, .pImageInfo=&image};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,layout,0,write);
    const auto vp=O::ProjectionReversedZ(fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown,0.001f)
                  * O::RigidInverse(Matrix(eye));
    for (const auto& p:pieces) {
        const auto model=p.view_space ? Matrix(render_head)*Matrix(p.pose) : Matrix(p.pose);
        const Push push{(vp*model).m,
            {float(p.rect.offset.x)/atlas_extent.width, float(p.rect.offset.y)/atlas_extent.height,
             float(p.rect.extent.width)/atlas_extent.width, float(p.rect.extent.height)/atlas_extent.height},
            {p.width,p.height,p.radius,p.angle},p.tint};
        ScratchPush::Invalidate();
        cmd.pushConstants(layout,vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                          0,sizeof(push),&push);
        cmd.draw(p.radius>0 ? 32*6 : 6,1,0,0);
    }
    cmd.endRendering();
}
} // namespace Vulkan
#endif
