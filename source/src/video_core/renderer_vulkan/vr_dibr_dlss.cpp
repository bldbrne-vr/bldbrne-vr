// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "video_core/renderer_vulkan/vr_dibr_dlss.h"
#include <charconv>
#include <string_view>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "core/vr/render_taa.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vr_camera_motion.h"
#include "video_core/renderer_vulkan/vr_temporal_images.h"
#include "video_core/renderer_vulkan/vr_temporal_runtime.h"

namespace Vulkan {
namespace {
vk::Extent2D OutputSize(vk::Extent2D input) {
    const char* value = std::getenv("VR_DIBR_DLSS_OUTPUT");
    if (!value) return input; // equal size is DLAA, explicitly logged
    const std::string_view text(value);
    const auto split = text.find('x');
    if (split == text.npos) return {};
    vk::Extent2D out{};
    const auto w = std::from_chars(text.data(), text.data() + split, out.width);
    const auto h = std::from_chars(text.data() + split + 1, text.data() + text.size(), out.height);
    if (w.ec != std::errc{} || h.ec != std::errc{} || w.ptr != text.data()+split ||
        h.ptr != text.data()+text.size() || out.width < input.width || out.height < input.height ||
        out.width > 8192 || out.height > 8192 ||
        std::abs(double(out.width)*input.height / (double(out.height)*input.width)-1.) > .005)
        return {};
    return out;
}
vk::Format LinearView(vk::Format f) {
    if (f == vk::Format::eR8G8B8A8Unorm) return vk::Format::eR8G8B8A8Srgb;
    if (f == vk::Format::eB8G8R8A8Unorm) return vk::Format::eB8G8R8A8Srgb;
    return f;
}
}
struct VrDibrDlss::State {
    const Instance& instance;
    Scheduler& scheduler;
    VrTemporalRuntime runtime;
    VrTemporalImages images;
    VrCameraMotion motion;
    VrMotionCamera::Camera previous{};
    std::array<float, 2> previous_jitter{};
    VR::RenderTaa::FrameFacts previous_facts{};
    uint64_t frame{}, resolved{};
    bool ready{}, logged{};
    VideoCore::UniqueImage encoded;
    vk::Extent2D encoded_size{};
    void ReleaseEncoded() {
        if (encoded) scheduler.DeferOperation([old=std::move(encoded)]() mutable { old.Destroy(); });
        encoded = VideoCore::UniqueImage(); encoded_size = vk::Extent2D{};
    }
    bool Encode(vk::Image source, vk::Extent2D size) {
        if (encoded_size != size) {
            ReleaseEncoded();
            encoded = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            encoded.image_ci = {.imageType=vk::ImageType::e2D, .format=vk::Format::eR8G8B8A8Srgb,
                .extent={size.width,size.height,1}, .mipLevels=1, .arrayLayers=1,
                .samples=vk::SampleCountFlagBits::e1, .tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
            const VmaAllocationCreateInfo allocation{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            VkImage image{};
            if (vmaCreateImage(instance.GetAllocator(),
                reinterpret_cast<const VkImageCreateInfo*>(&encoded.image_ci), &allocation,
                &image, &encoded.allocation, nullptr) != VK_SUCCESS) return false;
            encoded.image=image; encoded_size=size;
        }
        const auto cmd=scheduler.CommandBuffer();
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .oldLayout=vk::ImageLayout::eUndefined, .newLayout=vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=encoded.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        const std::array<vk::Offset3D,2> offsets{vk::Offset3D{0,0,0},
            vk::Offset3D{int32_t(size.width),int32_t(size.height),1}};
        const vk::ImageBlit blit{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .srcOffsets=offsets,.dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.dstOffsets=offsets};
        cmd.blitImage(source,vk::ImageLayout::eGeneral,encoded.image,
                      vk::ImageLayout::eTransferDstOptimal,blit,vk::Filter::eNearest);
        barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
        barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
        barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        return true;
    }
    State(const Instance& i, Scheduler& s): instance(i), scheduler(s), runtime(i,s), images(i,s), motion(i,s) {}
    ~State() { ReleaseEncoded(); }
};
VrDibrDlss::VrDibrDlss(const Instance& i, Scheduler& s): state(std::make_unique<State>(i,s)) {}
VrDibrDlss::~VrDibrDlss() = default;
bool VrDibrDlss::Ready() const { return DibrDlss::Requested() && state->ready; }
vk::Extent2D VrDibrDlss::DesiredSize(vk::Extent2D input) const {
    const auto output = Ready() ? OutputSize(input) : input;
    return output.width ? output : input;
}
VrDibrDlss::Output VrDibrDlss::Record(VideoCore::Image& color, VideoCore::Image& depth,
        vk::ComponentMapping components, const VR::CameraRecord& rec,
        float xs, float ys, bool already_resolved) {
    auto& s = *state;
    s.ready = false;
    const auto config = VR::Graphics::Accepted();
    if (!DibrDlss::Requested()) {
        auto off = config; off.upscaler = VR::Graphics::Upscaler::Off;
        s.runtime.Begin(off, {});
        s.images.Release();
        s.ReleaseEncoded();
        s.previous_facts.valid = false;
        return {};
    }
    const vk::Extent2D input{color.info.size.width, color.info.size.height};
    const auto output = OutputSize(input);
    if (!output.width || !rec.dibr || rec.menu || !rec.has_render_position ||
        !rec.has_projection || !(rec.near_z > 0 && rec.far_z > rec.near_z) ||
        depth.info.size.width != input.width || depth.info.size.height != input.height ||
        color.backing->num_samples != 1 || depth.backing->num_samples != 1) {
        s.previous_facts.valid = false;
        return {};
    }
    VrTemporalPair::Plan plan{};
    plan.views = 1;
    plan.eyes[0] = {input.width, input.height, output.width, output.height};
    const auto result = s.runtime.Begin(config, plan);
    if (result != VrTemporalPair::Result::Ready) {
        s.previous_facts.valid = false;
        return {};
    }
    s.ready = true;
    // The first ready frame may already have used TAA. Let that frame finish unchanged;
    // the AA hook bypasses it next frame, only after the provider became ready.
    if (already_resolved) return {};
    s.scheduler.EndRendering();
    color.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    depth.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    VideoCore::ImageViewInfo cv{}, dv{};
    cv.format = LinearView(color.backing->image.image_ci.format);
    cv.mapping = components;
    dv.format = vk::Format::eR32Sfloat;
    std::array<VrTemporalImages::Source,2> sources{};
    sources[0] = {*color.FindView(cv,false).image_view, *depth.FindView(dv,false).image_view,
                  input.width, input.height};
    if (!s.images.Prepare(config, true, plan, sources, false, 0)) {
        s.ready = false; return {};
    }
    namespace RT = VR::RenderTaa;
    const auto rows = RT::CameraRows(rec.base_yaw-rec.head_yaw, -rec.head_pitch, rec.head_roll);
    const std::array<float,3> pos{rec.render_position.x, rec.render_position.y, rec.render_position.z};
    VrMotionCamera::Camera camera{};
    for (unsigned r=0; r<3; ++r) {
        for (unsigned c=0; c<3; ++c) camera.view[4*r+c] = rows[3*r+c];
        for (unsigned c=0; c<3; ++c) camera.view[4*r+3] -= rows[3*r+c]*pos[c];
    }
    const auto inverse = VrMotionCamera::Inverse(camera.view);
    if (!inverse) { s.ready = false; return {}; }
    camera.inverse = *inverse;
    const float cot = 1.f/std::tan(rec.proj_vfov*.5f);
    const float aspect = rec.proj_aspect > 0 ? rec.proj_aspect : float(input.width)/input.height;
    camera.projection = {xs*cot/aspect, ys*cot, rec.far_z/(rec.far_z-rec.near_z),
                         -rec.near_z*rec.far_z/(rec.far_z-rec.near_z)};
    RT::FrameFacts facts{};
    facts.valid = true; facts.width = input.width; facts.height = input.height;
    facts.turn_serial = rec.turn_serial; facts.space_serial = rec.space_serial;
    facts.world_scale = rec.world_scale; facts.time_s = double(rec.tick_ns)*1e-9;
    facts.rows = rows; facts.pos = pos;
    const bool reset = RT::ResetReason(s.previous_facts, facts) != RT::Reset::None;
    const auto jitter = VR::FrameTaaJitter();
    const std::array<float,2> j{jitter.on ? jitter.px_x*xs : 0.f,
                                jitter.on ? jitter.px_y*ys : 0.f};
    auto frames = s.images.Images();
    const auto& f = frames[0];
    std::array<VrCameraMotion::Eye,2> motion{};
    motion[0] = {vk::ImageView(f.depth.view), {}, vk::ImageView(f.motion.view),
                  vk::Image(f.motion.image), VrMotionCamera::Build(camera,
                    reset ? camera : s.previous, input.width, input.height, j,
                    reset ? j : s.previous_jitter, false)};
    if (!s.motion.Record(config, true, motion, nullptr, 1)) { s.ready = false; return {}; }
    frames[0].jitter_x = j[0]; frames[0].jitter_y = j[1]; frames[0].reset = reset;
    frames[0].frame_ms = reset ? 16.667f : float((facts.time_s-s.previous_facts.time_s)*1000.);
    if (!(frames[0].frame_ms > 0)) frames[0].frame_ms = 16.667f;
    VrTemporalPair::Frame frame{}; frame.eyes = frames;
    if (s.runtime.Resolve(++s.frame, frame) != VrTemporalPair::Result::Ready) {
        s.ready = false; s.previous_facts.valid = false; return {};
    }
    if (!s.Encode(vk::Image(frames[0].output.image), output)) {
        s.ready=false; s.previous_facts.valid=false; return {};
    }
    s.previous = camera; s.previous_jitter = j; s.previous_facts = facts;
    VR::NoteTaaResolve(input.width, input.height, true);
    if (!s.logged || ++s.resolved % 300 == 0) {
        s.logged = true;
        LOG_INFO(Render_Vulkan, "VR DIBR DLSS experiment: {}x{} -> {}x{}, one center feature, "
                 "{}; camera-only motion, jitter={}, canonical buffers {} MiB, resolved={}",
                 input.width, input.height, output.width, output.height,
                 input == output ? "DLAA" : "Super Resolution", jitter.on,
                 (uint64_t(input.width)*input.height*16 + uint64_t(output.width)*output.height*8)>>20,
                 s.resolved);
    }
    // Copy the encoded bytes into DIBR's usual UNORM scene, preserving its UI/veil contract.
    return {s.encoded.image, output};
}
}
