#include "../../../scratchpad/dual-eye-perf/push_state_memo.h"
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_render_taa.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_render_taa_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// Mirrors TaaPush in vr_render_taa.comp.
struct TaaPush {
    s32 size[2];
    s32 depth_size[2];
    float jitter[2];
    float m[9];
    float t[3];
    float fx, fy, pfx, pfy;
    float da, db, dsky;
    float pda, pdb, pdsky;
    float alpha_min;
    float gamma;
    float depth_reject;
    u32 flags;
    u32 flags2; // round 2: the luma weight (bits 0..7), the occluder test (bit 8), the
                // sharpening's lift (bits 16..23)
};
static_assert(sizeof(TaaPush) == 132);

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr u32 Group = 16;                       // vr_render_taa.comp's local size
constexpr u64 TimerKey = 0x5441410000000001ull; // "TAA": the `VR gpu passes:` line (cs 5441...)
constexpr vk::Format HistFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::Format DepthFormat = vk::Format::eR32Sfloat;

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

} // Anonymous namespace

VrRenderTaa::VrRenderTaa(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()} {
    ready = Init();
}

VrRenderTaa::~VrRenderTaa() {
    for (const Views& v : views) {
        if (v.unorm) {
            device.destroyImageView(v.unorm);
        }
    }
}

void VrRenderTaa::Fail(const char* what) {
    LOG_ERROR(Render_Vulkan, "VR taa: unavailable ({}); aa_mode=taa uses SMAA instead", what);
    ready = false;
}

bool VrRenderTaa::Init() {
    auto s = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    });
    if (s.result != vk::Result::eSuccess) {
        Fail("sampler");
        return false;
    }
    sampler = std::move(s.value);

    const std::array<vk::DescriptorType, 7> types{
        vk::DescriptorType::eSampledImage,         // 0 B
        vk::DescriptorType::eSampledImage,         // 1 depth
        vk::DescriptorType::eCombinedImageSampler, // 2 history (previous)
        vk::DescriptorType::eStorageImage,         // 3 history (new)
        vk::DescriptorType::eStorageImage,         // 4 A
        vk::DescriptorType::eSampledImage,         // 5 stable depth (previous)
        vk::DescriptorType::eStorageImage,         // 6 stable depth (new)
    };
    std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = types[i],
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    auto sl = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    if (sl.result != vk::Result::eSuccess) {
        Fail("descriptor set layout");
        return false;
    }
    set_layout = std::move(sl.value);
    // 132 bytes of push constants: every desktop GPU allows 256 (Vulkan's minimum is 128).
    if (instance.GetPhysicalDevice().getProperties().limits.maxPushConstantsSize < sizeof(TaaPush)) {
        Fail("push constants (the device allows fewer than 132 bytes)");
        return false;
    }
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(TaaPush),
    };
    const vk::DescriptorSetLayout set_layout_handle = *set_layout;
    auto pl = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout_handle,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (pl.result != vk::Result::eSuccess) {
        Fail("pipeline layout");
        return false;
    }
    layout = std::move(pl.value);
    const auto make = [&](std::vector<std::string> defines, const char* name,
                          vk::UniquePipeline& out) {
        const vk::ShaderModule module = Compile(HostShaders::VR_RENDER_TAA_COMP,
                                                vk::ShaderStageFlagBits::eCompute, device,
                                                std::move(defines));
        if (!module) {
            return false;
        }
        const vk::ComputePipelineCreateInfo info{
            .stage{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = *layout,
        };
        auto [result, p] = device.createComputePipelineUnique({}, info);
        device.destroyShaderModule(module);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        out = std::move(p);
        SetObjectName(device, *out, std::string_view{name});
        return true;
    };
    if (!make({}, "vr_render_taa.comp", pipeline) ||
        !make({"TAA_SHARPEN"}, "vr_render_taa.comp (sharpen)", sharpen_pipeline)) {
        Fail("pipelines");
        return false;
    }
    LOG_INFO(Render_Vulkan, "VR taa: ready (render-space temporal AA, history {})",
             vk::to_string(HistFormat));
    return true;
}

bool VrRenderTaa::MakeTarget(Target& t, vk::Extent2D size, vk::Format format, const char* name) {
    if (t.image) {
        scheduler.DeferOperation([image = std::move(t.image), view = std::move(t.view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    t.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    t.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, static_cast<vk::Image>(t.image), std::string_view{name});
    auto v = device.createImageViewUnique({
        .image = t.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    });
    if (v.result != vk::Result::eSuccess) {
        Fail("image view");
        return false;
    }
    t.view = std::move(v.value);
    t.fresh = true;
    return true;
}

bool VrRenderTaa::EnsureTargets(vk::Extent2D size) {
    if (hist[0].image && extent == size) {
        return true;
    }
    for (u32 i = 0; i < 2; ++i) {
        if (!MakeTarget(hist[i], size, HistFormat, i == 0 ? "vr_taa history 0" : "vr_taa history 1") ||
            !MakeTarget(zst[i], size, DepthFormat, i == 0 ? "vr_taa depth 0" : "vr_taa depth 1")) {
            return false;
        }
    }
    extent = size;
    LOG_INFO(Render_Vulkan, "VR taa: {}x{} (history 2x RGBA16F + 2x R32F, {} MB)", size.width,
             size.height, HistoryBytes() >> 20);
    return true;
}

u64 VrRenderTaa::HistoryBytes() const {
    return static_cast<u64>(extent.width) * extent.height * (2 * 8 + 2 * 4);
}

vk::ImageView VrRenderTaa::StableDepthView() const {
    return zst[cur].view ? *zst[cur].view : vk::ImageView{};
}

const VrRenderTaa::Views* VrRenderTaa::ViewsOf(vk::Image image, u64 uid, bool storage) {
    for (Views& v : views) {
        if (v.image == image && v.uid == uid) {
            v.last_use = frame;
            return &v;
        }
    }
    std::erase_if(views, [&](const Views& v) {
        if (frame - v.last_use < 600) {
            return false;
        }
        scheduler.DeferOperation([dev = device, a = v.unorm] {
            if (a) {
                dev.destroyImageView(a);
            }
        });
        return true;
    });
    Views v{.image = image, .uid = uid, .last_use = frame};
    vk::ImageViewUsageCreateInfo usage{.usage = storage ? vk::ImageUsageFlagBits::eStorage
                                                        : vk::ImageUsageFlagBits::eSampled};
    auto un = device.createImageView({
        .pNext = &usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = ColorRange,
    });
    if (un.result != vk::Result::eSuccess) {
        return nullptr;
    }
    v.unorm = un.value;
    views.push_back(v);
    return &views.back();
}

bool VrRenderTaa::Record(vk::CommandBuffer cmdbuf, const Frame& f) {
    ++frame;
    const auto refuse = [&](u32 bit, const char* why) {
        if ((warned & bit) == 0) {
            warned |= bit;
            LOG_WARNING(Render_Vulkan, "VR taa: {}; such frames use SMAA", why);
        }
        return false;
    };
    if (!ready) {
        return false;
    }
    if (!f.src || !f.dst || f.src == f.dst || f.extent.width == 0 || f.extent.height == 0 ||
        !f.depth_view) {
        return refuse(1u << 0, "no FXAA texture / target / depth");
    }
    if (!EnsureTargets(f.extent)) {
        return false;
    }
    // A view of B and a storage view of A (the same bytes either way; the resolve does not care
    // about the red / blue order: YCoCg's Y is symmetric in them).
    const Views* src_views = ViewsOf(f.src, f.src_uid, false);
    if (src_views == nullptr) {
        return refuse(1u << 1, "no view of the FXAA texture");
    }
    const vk::ImageView src_view = src_views->unorm;
    const Views* dst_views = ViewsOf(f.dst, f.dst_uid, true);
    if (dst_views == nullptr) {
        return refuse(1u << 2, "no storage view of the FXAA target");
    }
    const vk::ImageView dst_view = dst_views->unorm;

    const u32 prev = cur;
    const u32 next = cur ^ 1u;
    const bool history = f.history && !hist[prev].fresh && !zst[prev].fresh;

    scheduler.GpuWorkBegin(TimerKey, f.extent.width, f.extent.height); // VR_GPU_PASSES only
    GpuPassProfiler::MarkAt(scheduler, TimerKey, GpuPassProfiler::Group::Aa,
                            "VR render TAA resolve (in place of the game's FXAA)");
    // The slots written now were read last frame (the resolve, the DIBR prep): overwritten
    // whole. The slots read now: valid layouts (written last frame, visible since its barrier;
    // or still fresh, never read for real: history off).
    std::array<vk::ImageMemoryBarrier2, 4> pre{};
    u32 npre = 0;
    for (Target* t : {&hist[next], &zst[next]}) {
        pre[npre++] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = t->image,
            .subresourceRange = ColorRange,
        };
        t->fresh = false;
    }
    for (Target* t : {&hist[prev], &zst[prev]}) {
        if (t->fresh) {
            pre[npre++] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = t->image,
                .subresourceRange = ColorRange,
            };
            t->fresh = false;
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = npre,
        .pImageMemoryBarriers = pre.data(),
    });

    const std::array<vk::DescriptorImageInfo, 7> infos{{
        {.imageView = src_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView = f.depth_view, .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal},
        {.sampler = *sampler,
         .imageView = *hist[prev].view,
         .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *hist[next].view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = dst_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *zst[prev].view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *zst[next].view, .imageLayout = vk::ImageLayout::eGeneral},
    }};
    const std::array<vk::DescriptorType, 7> types{
        vk::DescriptorType::eSampledImage,         vk::DescriptorType::eSampledImage,
        vk::DescriptorType::eCombinedImageSampler, vk::DescriptorType::eStorageImage,
        vk::DescriptorType::eStorageImage,         vk::DescriptorType::eSampledImage,
        vk::DescriptorType::eStorageImage,
    };
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = types[i],
            .pImageInfo = &infos[i],
        };
    }
    TaaPush push{};
    push.size[0] = static_cast<s32>(f.extent.width);
    push.size[1] = static_cast<s32>(f.extent.height);
    push.depth_size[0] = static_cast<s32>(f.depth_extent.width);
    push.depth_size[1] = static_cast<s32>(f.depth_extent.height);
    push.jitter[0] = f.jitter_x;
    push.jitter[1] = f.jitter_y;
    for (int i = 0; i < 9; ++i) {
        push.m[i] = f.reproj.m[i];
    }
    for (int i = 0; i < 3; ++i) {
        push.t[i] = f.reproj.t[i];
    }
    push.fx = f.fx;
    push.fy = f.fy;
    push.pfx = f.pfx;
    push.pfy = f.pfy;
    push.da = f.depth.a;
    push.db = f.depth.b;
    push.dsky = f.depth.sky;
    push.pda = f.prev_depth.a;
    push.pdb = f.prev_depth.b;
    push.pdsky = f.prev_depth.sky;
    push.alpha_min = f.alpha_min;
    push.gamma = f.gamma;
    push.depth_reject = f.depth_reject;
    // taa_sharpen (not with the history map): A is written by the sharpen pass.
    const bool sharpen = f.sharpen > 0.0f && f.debug != 1;
    const u32 amount = static_cast<u32>(std::clamp(f.sharpen, 0.0f, 1.0f) * 255.0f + 0.5f);
    push.flags = (history ? 1u : 0u) | (f.y_down ? 2u : 0u) | ((f.debug & 3u) << 2) |
                 (sharpen ? 16u : 0u) | (VR::RenderTaa::FilterCode(f.filter_px) << 5) |
                 (amount << 8) | VR::RenderTaa::NearCode(f.near_m, f.near_alpha);
    push.flags2 = VR::RenderTaa::LumaCode(f.luma_weight) | (f.occluder ? 256u : 0u) |
                  (VR::RenderTaa::LiftCode(f.sharpen_lift) << 16);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    ScratchPush::Invalidate();
    cmdbuf.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch(DivCeil(f.extent.width, Group), DivCeil(f.extent.height, Group), 1);

    // The new history and stable depth: read by the next resolve and this frame's DIBR prep.
    std::array<vk::ImageMemoryBarrier2, 2> post{};
    u32 npost = 0;
    for (Target* t : {&hist[next], &zst[next]}) {
        post[npost++] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = t->image,
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = npost,
        .pImageMemoryBarriers = post.data(),
    });
    if (sharpen) {
        // RCAS of the new history into A (B for its alpha; the game's depth: the sharpening
        // fades out in the near field, where the weapon is mostly this frame's jittered image).
        const std::array<vk::DescriptorImageInfo, 4> sinfos{{
            {.imageView = src_view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
            {.sampler = *sampler,
             .imageView = *hist[next].view,
             .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = dst_view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = f.depth_view,
             .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal},
        }};
        const std::array<vk::WriteDescriptorSet, 4> swrites{{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &sinfos[0]},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
             .pImageInfo = &sinfos[1]},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &sinfos[2]},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &sinfos[3]},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, swrites);
        ScratchPush::Invalidate();
        cmdbuf.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmdbuf.dispatch(DivCeil(f.extent.width, Group), DivCeil(f.extent.height, Group), 1);
    }
    scheduler.GpuWorkEnd();
    cur = next;
    if (frame == 1) {
        LOG_INFO(Render_Vulkan,
                 "VR taa: first frame {}x{} ({} -> {}, depth read {}x{}, {}x{} groups)",
                 f.extent.width, f.extent.height, vk::to_string(f.src_format),
                 vk::to_string(f.dst_format), f.depth_extent.width, f.depth_extent.height,
                 DivCeil(f.extent.width, Group), DivCeil(f.extent.height, Group));
    }
    return true;
}

} // namespace Vulkan
