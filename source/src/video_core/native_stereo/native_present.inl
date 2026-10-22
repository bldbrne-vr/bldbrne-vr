// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Native stereo: crop/format-copy the two rendered layers; no DIBR reconstruction.
void Presenter::RecordNativeStereo(vk::CommandBuffer cmd, Frame& frame, VideoCore::Image& source, vk::Extent2D size, vk::Format guest_view_format) {
    if (!openxr_presenter || !frame.vr_tag.immersive || !source.native_stereo) return;
    const auto format=openxr_presenter->EyeSwapchainFormat();
    if (format==vk::Format::eUndefined) {
        RENDER_DIAGNOSTIC_ONCE("native eye format unavailable (may be session startup)");
        return;
    }
    // The guest flip's view describes the stored colour bytes, not the backing image's
    // typeless allocation. A typed blit from that allocation swaps R/B a second time.
    const bool convert_format = format != guest_view_format;
    static vk::Format logged_guest_format = vk::Format::eUndefined;
    static vk::Format logged_xr_format = vk::Format::eUndefined;
    if (guest_view_format != logged_guest_format || format != logged_xr_format) {
        LOG_INFO(Render_Vulkan, "VR native stereo formats: guest {}, XR {}, {}",
                 vk::to_string(guest_view_format), vk::to_string(format),
                 convert_format ? "typed colour conversion" : "direct copy eligible");
        logged_guest_format = guest_view_format;
        logged_xr_format = format;
    }
    // The pre-UI capture contains the two world views already rendered by the game. With S1
    // (FAST bit 51) it is the reconstruction at the output size, larger than the guest frame;
    // a frame without it shows the guest frame at its own (input) size.
    const auto scene = vr_ui_layer->Scene();
    const auto whole = VR::Graphics::S1Output();
    const bool ui_free = frame.vr_ui.valid && scene.image && scene.layers == 2 &&
                         (scene.extent == size ||
                          (whole.width && scene.extent == vk::Extent2D{whole.width, whole.height}));
    const vk::Extent2D world_size = ui_free ? scene.extent : size;
    if (ui_free) {
        RENDER_DIAGNOSTIC_ONCE("native scene source={}x{} layers={} guest_format={} eye_format={} upscale={}x{}",
            world_size.width, world_size.height, scene.layers, static_cast<int>(guest_view_format),
            static_cast<int>(format), whole.width, whole.height);
    } else {
        RENDER_DIAGNOSTIC_ONCE("native guest fallback={}x{} ui_valid={} scene_present={} scene_layers={} scene={}x{}",
            size.width, size.height, frame.vr_ui.valid, bool(scene.image), scene.layers,
            scene.extent.width, scene.extent.height);
    }
    // A successful resolve can still be discarded when UI extraction is incomplete.
    // Log source changes so diagnostics distinguish that from a DLSS input failure.
    if (whole.width) {
        static const bool trace = std::getenv("VR_TEMPORAL_TRACE") != nullptr;
        static int previous_source = -1;
        const bool reconstructed = ui_free && scene.extent == vk::Extent2D{whole.width, whole.height};
        const int selected = reconstructed ? 2 : ui_free ? 1 : 0;
        // Both publication paths reject held frames; the headset keeps its previous
        // stereo pair. Do not report their unused candidate as a displayed fallback,
        // or let it replace the last source that was eligible for publication.
        if (trace && frame.vr_tag.hold) {
            LOG_INFO(Render_Vulkan,
                     "VR temporal held frame: keeping previous stereo pair; candidate={} "
                     "size={}x{} ui_valid={} class={}",
                     selected, world_size.width, world_size.height, frame.vr_ui.valid,
                     frame.vr_tag.ui_class);
        } else if (trace && selected != previous_source) {
            previous_source = selected;
            const auto& ui = rasterizer->VrUiFrame();
            const auto status = VR::Graphics::UnpackStatus(
                VR::Graphics::run_status.load(std::memory_order_relaxed));
            LOG_WARNING(Render_Vulkan,
                "VR temporal presented source: selected={} size={}x{} status={} ui_valid={} "
                "class={} marker={} armed={} incomplete={} refusal={} scene={}x{} layers={}",
                selected, world_size.width, world_size.height, static_cast<u32>(status.state),
                frame.vr_ui.valid, frame.vr_tag.ui_class, ui.marker, ui.armed, ui.incomplete,
                ui.refused ? ui.refused : "none", scene.extent.width, scene.extent.height, scene.layers);
        }
    }

    auto crops=openxr_presenter->EyeCropsFor(frame.vr_tag.record,world_size);
    if (NativeStereo::AsymmetricProjection()) {
        const auto& record = frame.vr_tag.record;
        const float tan_y = std::tan(record.vfov*.5f);
        const float tan_x = tan_y*record.aspect;
        bool valid = true;
        for (const auto& f : record.eye_fov)
            valid &= NativeProjection::Make({f.angle_left,f.angle_right,f.angle_up,f.angle_down},
                                            tan_x,tan_y).valid;
        if (valid) {
            for (u32 eye=0;eye<2;++eye)
                crops[eye] = {record.eye_fov[eye], {{0,0},world_size}};
        }
    }
    if (!openxr_presenter->DibrEyesFit({crops[0].rect.extent,crops[1].rect.extent})) {
        RENDER_DIAGNOSTIC_ONCE("native eyes do not fit: L={}x{} R={}x{}",
            crops[0].rect.extent.width, crops[0].rect.extent.height,
            crops[1].rect.extent.width, crops[1].rect.extent.height);
        return;
    }
    // The old feedback path runs in DIBR late warp. Native eyes need their own
    // optional pass; take one game-state sample and apply identical strengths to both eyes.
    const auto& settings = VR::GetSettings();
    const auto color = VR::Color::Get();
    VR::VitalColor::Output feedback{};
    if (settings.health_color || settings.stamina_color || native_feedback_ns != 0) {
        const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        VR::Vitals::Status status{};
        VR::Vitals::LatestStatus(status);
        const bool gameplay = frame.vr_tag.record.driven && !frame.vr_tag.record.menu &&
                              frame.vr_tag.ui_class == static_cast<u8>(VR::UiClass::None) &&
                              !VR::IsGameMenuOpen() && !VR::MenuPanelActive() &&
                              !VR::CinematicActive() && !VR::Page::Runtime::Get().IsOpen();
        feedback = native_feedback_state.Update(status,
            {settings.health_color, settings.stamina_color,
             settings.health_color_strength, settings.stamina_color_strength},
            gameplay, now, native_feedback_ns ? float(now - native_feedback_ns) * 1e-9f : 0.0f);
        native_feedback_ns = (settings.health_color || settings.stamina_color) ? now : 0;
    }
    const bool process = convert_format || feedback.health > .0001f ||
                         feedback.desaturation > .0001f || color.Active();
    if (process) {
        if (!native_feedback_pass) {
            native_feedback_pass = std::make_unique<HostPasses::PostProcessingPass>();
            native_feedback_pass->Create(instance.GetDevice(), format, true);
            native_feedback_format = format;
        }
        ASSERT(format == native_feedback_format);
    }
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    using L = vk::ImageLayout;
    const auto read_stage = process ? S::eFragmentShader : S::eTransfer;
    const auto read_access = process ? A::eShaderSampledRead : A::eTransferRead;
    const auto read_layout = process ? L::eShaderReadOnlyOptimal : L::eTransferSrcOptimal;
    // Only the UI draws were replayed into K/W; never put a copy of the world on the menu.
    GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0010ull,
                            GpuPassProfiler::Group::NativeStereo,
                            "VR native stereo: eye crop copies from the layered frame");
    vk::Image world = source.GetImage();
    if (ui_free) {
        world = scene.image;
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = read_stage,
            .dstAccessMask = read_access,
            .oldLayout = scene.layout, .newLayout = read_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = world, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    } else {
        frame.vr_ui = {}; // the fallback eyes already contain the game's UI
        source.Transit(read_layout, read_access, {}, cmd);
    }
    std::array<vk::ImageView, 2> input_views{};
    if (process) {
        if (ui_free) {
            input_views = vr_ui_layer->NativeSceneViews(guest_view_format);
        } else {
            VideoCore::ImageViewInfo view_info{};
            view_info.format = guest_view_format;
            const auto& view = source.FindView(view_info);
            input_views = {*view.image_view, *view.native_right};
        }
    }
    if (process && (!input_views[0] || !input_views[1])) {
        RENDER_DIAGNOSTIC_ONCE("native processing input missing: left={} right={} ui_free={}",
            bool(input_views[0]), bool(input_views[1]), ui_free);
    }
    static const bool fsr1_probe = std::getenv("VR_NATIVE_FSR1_PROBE") != nullptr;
    OpenXRPresenter::StereoEyes stereo{};
    stereo.image_format = format;
    for (u32 eye=0;eye<2;++eye) {
        const auto crop=crops[eye].rect;
        ASSERT(crop.offset.x>=0 && crop.offset.y>=0 && u32(crop.offset.x)+crop.extent.width<=world_size.width && u32(crop.offset.y)+crop.extent.height<=world_size.height);
        auto& target=frame.native_eyes[eye];
        if (!target || target->image_ci.extent.width!=crop.extent.width || target->image_ci.extent.height!=crop.extent.height || target->image_ci.format!=format) {
            // A Frame is only reused once its presentation work is complete.
            if (native_fsr1_probe) native_fsr1_probe->ReleaseInput(frame.id, eye);
            frame.native_eye_views[eye].reset();
            target=std::make_unique<VideoCore::UniqueImage>(instance.GetDevice(),instance.GetAllocator());
            target->Create(vk::ImageCreateInfo{
                .flags=fsr1_probe ? vk::ImageCreateFlagBits::eMutableFormat : vk::ImageCreateFlags{},
                .imageType=vk::ImageType::e2D,.format=format,
                .extent={crop.extent.width,crop.extent.height,1},.mipLevels=1,.arrayLayers=1,
                .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eColorAttachment,
                .initialLayout=vk::ImageLayout::eUndefined});
        }
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask=process ? S::eColorAttachmentOutput : S::eTransfer,
            .dstAccessMask=process ? A::eColorAttachmentWrite : A::eTransferWrite,
            .oldLayout=L::eUndefined,
            .newLayout=process ? L::eColorAttachmentOptimal : L::eTransferDstOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=target->image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        if (process) {
            auto& view = frame.native_eye_views[eye];
            if (!view) {
                view = Check<"native output image view">(instance.GetDevice().createImageViewUnique({
                    .image = target->image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = format,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                }));
            }
            Frame output{};
            output.width = crop.extent.width;
            output.height = crop.extent.height;
            output.image = target->image;
            output.image_view = *view;
            native_feedback_pass->Render(cmd, input_views[eye], world_size, output,
                {.gamma = 1.0f, .hdr = 1,
                 .health = feedback.health, .desaturation = feedback.desaturation,
                 .color_adjust = color.Push(),
                 .source_offset = {crop.offset.x, crop.offset.y}});
        } else {
            const vk::ImageCopy region{
                .srcSubresource={vk::ImageAspectFlagBits::eColor,0,eye,1},
                .srcOffset={crop.offset.x,crop.offset.y,0},
                .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .dstOffset={0,0,0},.extent={crop.extent.width,crop.extent.height,1}};
            cmd.copyImage(world,vk::ImageLayout::eTransferSrcOptimal,target->image,
                          vk::ImageLayout::eTransferDstOptimal,region);
        }
        barrier.srcStageMask=process ? S::eColorAttachmentOutput : S::eTransfer;
        barrier.srcAccessMask=process ? A::eColorAttachmentWrite : A::eTransferWrite;
        barrier.dstStageMask=S::eTransfer;
        barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
        barrier.oldLayout=process ? L::eGeneral : L::eTransferDstOptimal;
        barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        frame.vr_eye_image[eye]=target->image;frame.vr_eye_extent[eye]=crop.extent;
        frame.vr_eye_rect[eye]=crop;frame.vr_eye_fov[eye]=crops[eye].fov;
        stereo.image[eye]=target->image;stereo.extent[eye]=crop.extent;stereo.crop[eye]=crops[eye];
    }
    if (fsr1_probe && !frame.vr_tag.hold) {
        const std::array<vk::Extent2D,2> in{stereo.extent[0],stereo.extent[1]};
        // Exact headset target, rather than FSR1's old common-factor rounding.
        const auto l=VR::Graphics::Headset(0), r=VR::Graphics::Headset(1);
        const std::array<vk::Extent2D,2> out{{{l.width,l.height},{r.width,r.height}}};
        if (!native_fsr1_probe) native_fsr1_probe=std::make_unique<VrNativeFsr1Probe>(instance.GetDevice(),instance.GetAllocator());
        std::array<vk::Image,2> images{};
        GpuPassProfiler::MarkAt(draw_scheduler,0xF11F'0000'0030ull,GpuPassProfiler::Group::VrHost,"Native FSR1 EASU/RCAS probe");
        if (native_fsr1_probe->Record(cmd,frame.id,stereo.image,in,out,VR::GetSettings().eye_sharpen,
                                     format==vk::Format::eB8G8R8A8Srgb,images)) {
            for (u32 e=0;e<2;++e) { stereo.image[e]=images[e];stereo.extent[e]=out[e];stereo.crop[e].rect={{0,0},out[e]}; }
            static u64 spatial_frames=0;
            if (++spatial_frames%300==0) LOG_INFO(Render_Vulkan,"Native FSR1 probe: {}x{} -> {}x{} per eye",in[0].width,in[0].height,out[0].width,out[0].height);
        }
    }
    frame.vr_stereo=true;frame.vr_eye_offset_m=.5f*NativeStereo::Ipd(frame.vr_tag.record);
    frame.vr_eye_bgra=format==vk::Format::eB8G8R8A8Srgb;
    stereo.eye_offset_m=frame.vr_eye_offset_m;stereo.native=true;
    if (!frame.vr_tag.hold) {
        GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0012ull,
                                GpuPassProfiler::Group::VrHost,
                                "VR eye copy into the XR swapchains");
        frame.vr_published=openxr_presenter->RecordStereoCopy(cmd,stereo,frame.vr_tag);
    }
    if (frame.vr_published) {
        RENDER_DIAGNOSTIC_ONCE("native eye copy recorded (not pixel validation): L={}x{} R={}x{} process={} ui_free={}",
            stereo.extent[0].width, stereo.extent[0].height, stereo.extent[1].width,
            stereo.extent[1].height, process, ui_free);
        // The atlas and both world eyes are published as one frame, with the existing UI poses.
        GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0013ull,
                                GpuPassProfiler::Group::VrHost,
                                "VR UI atlas copy into its XR swapchain");
        openxr_presenter->RecordUiCopy(cmd, frame.vr_ui, frame.vr_tag);
    }
    if (ui_free && !process) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal, .newLayout = scene.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = world, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    } else if (!ui_free) {
        source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,vk::AccessFlagBits2::eShaderRead,{},cmd);
    }
    static u64 count=0;
    if (++count%300==0) LOG_BENCHMARK_INFO(Render_Vulkan,"VR native stereo: {} pairs, {}x{} per-eye source, IPD {:.1f} mm, multiview colour, direct copy {}",
        count,size.width,size.height,NativeStereo::Ipd(frame.vr_tag.record)*1000.f,frame.vr_published);
}
