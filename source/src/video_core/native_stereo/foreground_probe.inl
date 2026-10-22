// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Diagnostic only: replay selected foreground into private stereo depth targets.
// Retains the original fragment shader (including alpha discard), so this is not
// an optimized depth-only shader benchmark. The displayed image stays unchanged.
struct ForegroundProbe {
    static constexpr u32 Width = 1568, Height = 1696, Slots = 16, Queries = 512;
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView array_view;
        std::array<vk::UniqueImageView, 2> eyes;
        bool initialized{};
        bool clear[2]{true, true};
    };
    std::array<Target, 2> targets;
    vk::UniqueQueryPool queries;
    std::array<u64, Slots> ticks{};
    u64 frame{};
    u32 count{}, draws{}, slot{};
    double cpu_us{};
    bool started{};
    u32 mode{};
    float timestamp_ns{};
};

// Read once at start-up: VrProbePrepare runs per indexed draw, and a function-local static's guard
// is a thread-local read per call.
static const u32 g_foreground_probe_mode = [] {
    const char* e = std::getenv("VR_FOREGROUND_PROBE");
    return e ? static_cast<u32>(std::clamp(std::atoi(e), 0, 3)) : 0u;
}();

void Rasterizer::VrProbePrepare(const GraphicsPipeline* pipeline) {
    const u32 mode = g_foreground_probe_mode;
    if (!mode || !probe_selected) { probe_selected = false; return; }
    const auto& key = pipeline->GetGraphicsKey();
    if (key.num_samples != 1 || key.prim_type != AmdGpu::PrimitiveType::TriangleList ||
        key.stage_hashes[u32(Shader::LogicalStage::Geometry)] ||
        key.stage_hashes[u32(Shader::LogicalStage::TessellationEval)] ||
        Cape().body_split || Cape().split) {
        probe_selected = false;
        return;
    }
    // Replaying a shader with guest-visible side effects is not safe.
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) continue;
        for (const auto& b : stage->buffers) if (b.is_written) { probe_selected = false; return; }
        for (const auto& i : stage->images) if (i.is_written || i.is_atomic) { probe_selected = false; return; }
    }
    if (!foreground_probe) {
        const auto features = instance.GetPhysicalDevice().getFeatures2<vk::PhysicalDeviceFeatures2,
            vk::PhysicalDeviceVulkan11Features>();
        const auto props = instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2,
            vk::PhysicalDeviceMultiviewProperties>();
        const bool supported = features.get<vk::PhysicalDeviceVulkan11Features>().multiview &&
            props.get<vk::PhysicalDeviceMultiviewProperties>().maxMultiviewViewCount >= 2;
        ASSERT_MSG(supported, "Foreground probe requires multiview with two views");
        foreground_probe = std::make_shared<ForegroundProbe>();
        auto& p = *foreground_probe;
        p.mode = mode;
        p.timestamp_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        auto [result, pool] = instance.GetDevice().createQueryPoolUnique({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = ForegroundProbe::Slots * ForegroundProbe::Queries,
        });
        ASSERT(result == vk::Result::eSuccess);
        p.queries = std::move(pool);
        LOG_INFO(Render_Vulkan, "VR foreground init: mode={} multiview={} maxViews={} extent={}x{} IPD=64mm source=2720x1808/vfov116; original FS retained; displayed image unchanged",
            mode, supported, props.get<vk::PhysicalDeviceMultiviewProperties>().maxMultiviewViewCount,
            ForegroundProbe::Width, ForegroundProbe::Height);
    }
    for (u32 i=0; i<2; ++i) {
        probe_pipelines[i] = (mode == 3 || mode == i+1)
            ? pipeline_cache.GetForegroundProbePipeline(pipeline, i+1) : nullptr;
    }
}

void Rasterizer::VrProbeDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                             s32 vertex_offset, u32 instance_offset) {
    if (!foreground_probe || !probe_selected) return;
    auto& p = *foreground_probe;
    // Projection coefficients below are only valid for this controlled fixture.
    if (state.width != 2720 || state.height != 1808 || !state.depth_stencil_attachment.has_depth) return;
    const auto begin = std::chrono::steady_clock::now();
    auto cb = scheduler.CommandBuffer();
    scheduler.EndRendering();
    if (!p.started) {
        p.slot = p.frame % ForegroundProbe::Slots;
        if (p.ticks[p.slot] && !scheduler.IsFree(p.ticks[p.slot])) {
            LOG_ERROR(Render_Vulkan, "VR foreground: query ring busy, rejecting frame {}", p.frame);
            return;
        }
        cb.resetQueryPool(*p.queries, p.slot * ForegroundProbe::Queries, ForegroundProbe::Queries);
        for (auto& target : p.targets) target.clear[0] = target.clear[1] = true;
        p.started = true;
    }
    ASSERT_MSG(p.count + 2 <= ForegroundProbe::Queries, "Foreground probe query capacity exceeded");
    cb.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *p.queries,
                      p.slot * ForegroundProbe::Queries + p.count++);
    auto& dynamic = scheduler.GetDynamicState();
    const auto old_viewports = dynamic.viewports;
    const auto old_scissors = dynamic.scissors;
    const auto old_push = push_data;
    dynamic.SetViewports(Viewports{{0.f, old_viewports[0].height < 0 ? float(ForegroundProbe::Height) : 0.f,
                                    float(ForegroundProbe::Width), old_viewports[0].height < 0 ? -float(ForegroundProbe::Height) : float(ForegroundProbe::Height),
                                    old_viewports[0].minDepth, old_viewports[0].maxDepth}});
    dynamic.SetScissors(Scissors{{{0,0}, {ForegroundProbe::Width, ForegroundProbe::Height}}});
    dynamic.SetDepthTestEnabled(true);
    dynamic.SetDepthWriteEnabled(true);
    dynamic.SetDepthCompareOp(vk::CompareOp::eLessOrEqual);
    dynamic.SetStencilTestEnabled(false);
    dynamic.SetDepthBoundsTestEnabled(false);
    dynamic.SetDepthBiasEnabled(false);
    dynamic.SetAttachmentFeedbackLoopEnabled(false);
    // Eye camera translations in the source clip coordinates. Preserve game units.
    constexpr float SourceM00 = 0.62486935f / (2720.f / 1808.f); // cot(116deg/2)
    const float shift = 0.032f * Cape().units_per_m * SourceM00;
    const auto issue = [&] {
        const auto& cs = Cape();
        const auto instances = std::as_const(liverpool->regs).num_instances.NumInstances();
        if (cs.hands_split) {
            for (u32 r=0; r<cs.hands_split_count; ++r)
                cb.drawIndexed(cs.hands_split[r].count, instances, cs.hands_split[r].first,
                               vertex_offset, instance_offset);
        } else {
            cb.drawIndexed(std::as_const(liverpool->regs).num_indices, instances, 0, vertex_offset, instance_offset);
        }
    };
    for (u32 variant=0; variant<2; ++variant) {
        const auto* probe = probe_pipelines[variant];
        if (!probe) continue;
        auto& t = p.targets[variant];
        if (!t.initialized) {
            t.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            t.image.Create({.imageType=vk::ImageType::e2D, .format=vk::Format::eD32Sfloat,
                .extent={ForegroundProbe::Width,ForegroundProbe::Height,1}, .mipLevels=1,
                .arrayLayers=2, .samples=vk::SampleCountFlagBits::e1, .tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferSrc});
            for (u32 eye=0; eye<3; ++eye) {
                auto [r,v] = instance.GetDevice().createImageViewUnique({.image=t.image,
                    .viewType=eye==2 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
                    .format=vk::Format::eD32Sfloat,
                    .subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,eye==2 ? 0u:eye,eye==2 ? 2u:1u}});
                ASSERT(r == vk::Result::eSuccess);
                if (eye==2) t.array_view=std::move(v); else t.eyes[eye]=std::move(v);
            }
        }
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask=vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            .srcAccessMask=t.initialized ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite : vk::AccessFlags2{},
            .dstStageMask=vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask=vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout=t.initialized ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eUndefined,
            .newLayout=vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=t.image, .subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,0,2}};
        cb.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        t.initialized=true;
        for (u32 eye=0; eye<(variant==1 ? 1u:2u); ++eye) {
            push_data.vr_shift = eye==0 ? shift : -shift;
            push_data.vr_hud = 0;
            probe->BindResources(set_writes, {}, push_data, vr_push);
            RenderState target{};
            target.width=ForegroundProbe::Width; target.height=ForegroundProbe::Height;
            target.num_layers=1; target.probe_view_mask=variant==1 ? 3:0;
            target.depth_stencil_attachment.image_view=variant==1 ? *t.array_view : *t.eyes[eye];
            target.depth_stencil_attachment.image_layout=vk::ImageLayout::eDepthStencilAttachmentOptimal;
            target.depth_stencil_attachment.has_depth=true;
            target.depth_stencil_attachment.depth_clear=t.clear[eye];
            target.depth_stencil_attachment.clear_value[0]=std::bit_cast<u32>(1.f);
            scheduler.BeginRendering(target);
            dynamic.Commit(instance, scheduler.Commands());
            cb.bindPipeline(vk::PipelineBindPoint::eGraphics, probe->Handle());
            issue();
            scheduler.EndRendering();
            t.clear[eye]=false;
        }
    }
    push_data=old_push;
    dynamic.SetViewports(old_viewports);
    dynamic.SetScissors(old_scissors);
    UpdateDynamicState(pipeline, true);
    pipeline->BindResources(set_writes, {}, push_data, vr_push); // the draw's own push again
    cb.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *p.queries,
                      p.slot * ForegroundProbe::Queries + p.count++);
    ++p.draws;
    p.cpu_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-begin).count();
}

void Rasterizer::VrProbeEndFrame() {
    if (!foreground_probe) return;
    auto shared=foreground_probe;
    auto& p=*shared;
    const char* dump_dir = std::getenv("VR_FOREGROUND_DUMP_DIR");
    if (p.started && dump_dir && std::filesystem::exists(std::filesystem::path(dump_dir)/"foreground_dump")) {
        scheduler.EndRendering();
        const auto cb=scheduler.CommandBuffer();
        const auto allocator=instance.GetAllocator();
        constexpr VkDeviceSize bytes=VkDeviceSize(ForegroundProbe::Width)*ForegroundProbe::Height*2*sizeof(float);
        for (u32 variant=0; variant<2; ++variant) {
            auto& t=p.targets[variant];
            if (!t.initialized) continue;
            VkBuffer buffer{};
            VmaAllocation allocation{};
            VmaAllocationInfo info{};
            const VkBufferCreateInfo bci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=bytes,
                .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            const VmaAllocationCreateInfo aci{.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage=VMA_MEMORY_USAGE_AUTO};
            ASSERT(vmaCreateBuffer(allocator,&bci,&aci,&buffer,&allocation,&info)==VK_SUCCESS);
            vk::ImageMemoryBarrier2 barrier{
                .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask=vk::AccessFlagBits2::eTransferRead,
                .oldLayout=vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .newLayout=vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=t.image,
                .subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,0,2}};
            cb.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
            cb.copyImageToBuffer(t.image,vk::ImageLayout::eTransferSrcOptimal,buffer,
                vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eDepth,0,0,2},
                    .imageExtent={ForegroundProbe::Width,ForegroundProbe::Height,1}});
            const vk::BufferMemoryBarrier2 host_barrier{
                .srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .buffer=buffer, .offset=0, .size=bytes};
            cb.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&host_barrier});
            barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask=vk::AccessFlagBits2::eTransferRead;
            barrier.dstStageMask=vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
            barrier.dstAccessMask=vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
            std::swap(barrier.oldLayout,barrier.newLayout);
            cb.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
            const auto path=std::filesystem::path(dump_dir)/fmt::format("foreground_{}_f{}.f32",variant+1,p.frame);
            scheduler.DeferOperation([allocator,buffer,allocation,info,path] {
                vmaInvalidateAllocation(allocator,allocation,0,bytes);
                std::ofstream out(path,std::ios::binary);
                out.write(static_cast<const char*>(info.pMappedData),bytes);
                out.close();
                vmaDestroyBuffer(allocator,buffer,allocation);
                LOG_INFO(Render_Vulkan,"VR foreground dump: {}",path.string());
            });
        }
        std::filesystem::remove(std::filesystem::path(dump_dir)/"foreground_dump");
    }
    if (p.started && p.count) {
        const u32 count=p.count, slot=p.slot, draws=p.draws;
        const u64 frame=p.frame;
        const double cpu=p.cpu_us;
        p.ticks[slot]=scheduler.CurrentTick();
        const auto device=instance.GetDevice();
        scheduler.DeferOperation([shared, device, count, slot, draws, frame, cpu] {
            std::vector<u64> timestamps(count);
            const auto result=device.getQueryPoolResults(*shared->queries, slot*ForegroundProbe::Queries,
                count, timestamps.size()*sizeof(u64), timestamps.data(), sizeof(u64), vk::QueryResultFlagBits::e64);
            if (result != vk::Result::eSuccess) {
                LOG_ERROR(Render_Vulkan, "VR foreground: timestamps unavailable for frame {}", frame);
                return;
            }
            u64 ticks=0;
            for (u32 i=0; i<count; i+=2) ticks+=timestamps[i+1]-timestamps[i];
            LOG_INFO(Render_Vulkan, "VR foreground: mode={} frame={} draws={} cpu_us={:.3f} gpu_ms={:.6f}",
                shared->mode, frame, draws, cpu, ticks*shared->timestamp_ns*1e-6);
        });
    }
    p.started=false; p.count=0; p.draws=0; p.cpu_us=0; ++p.frame;
}
