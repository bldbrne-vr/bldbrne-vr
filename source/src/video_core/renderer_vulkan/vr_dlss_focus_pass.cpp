// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/push_state_memo.h"
#include "video_core/renderer_vulkan/vr_dlss_focus_pass.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_dlss_focus_comp.h"
#include "video_core/host_shaders/vr_dlss_focus_crop_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// Mirrors CropPush in vr_dlss_focus_crop.comp.
struct CropPush {
    s32 rect[4];
};
/// Mirrors FocusPush in vr_dlss_focus.comp.
struct FocusPush {
    s32 rect[4];
    float map[4];
    float inv[4];
    s32 size[4];
    s32 origin[4];
    s32 frame[4];
};
static_assert(sizeof(CropPush) == 16 && sizeof(FocusPush) == 96);

constexpr u32 Group = 8; // both shaders' local size
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
// The blend band inside DLSS's rectangle, in eye pixels (VR_DLSS_FOCUS_BAND for tests).
constexpr float DefaultBand = 48.0f;
// This frame's share in the TAA outside the rectangle (VR_DLSS_FOCUS_TAA for tests, 0.02 to 1;
// 1: no TAA), on average: 0.06 keeps the last 94% (the still-head flicker mock, 2026-11-06: of
// 0.1 and 0.06, 0.06 changed fewest pixels visibly from moment to moment).
constexpr float DefaultTaa = 0.06f;
// The share of this frame's sample outside the rectangle taken from its render's 3x3 mean
// (VR_DLSS_FOCUS_SOFT for tests, 0 to 1): steadier where foveation shades coarsely (0.75: of 0.5
// and 0.75, the fewer visible changes in the still-head flicker mock, 2026-11-06).
constexpr float DefaultSoft = 0.75f;
// The history box's least half-width, in 8-bit steps of brightness (VR_DLSS_FOCUS_FLOOR for tests,
// 0 to 32): a flat neighbourhood (foveation's coarse blocks) still lets the TAA steady it. The
// still-head flicker mock (2026-11-06, Medium, outside the rectangle, pixels changing by 6 steps or
// more between moments): whole-eye DLSS 0.28%, no floor 1.10%, 4 steps 0.32%, 8 steps 0.08%.
constexpr float DefaultFloor = 8.0f;

// VR_DLSS_FOCUS_JITTER (tests): how the jitter moves the render's samples, +1 (texel q holds the
// scene at q + 0.5 - jitter, as render_taa's), -1 or 0 (left in).
float JitterSign() {
    static const float sign = [] {
        const char* v = std::getenv("VR_DLSS_FOCUS_JITTER");
        return !v ? 1.0f : v[0] == '-' ? -1.0f : v[0] == '0' ? 0.0f : 1.0f;
    }();
    return sign;
}

float TaaWeight() {
    static const float weight = [] {
        const char* v = std::getenv("VR_DLSS_FOCUS_TAA");
        const float w = v ? std::strtof(v, nullptr) : DefaultTaa;
        return w >= 0.02f && w <= 1.0f ? w : DefaultTaa;
    }();
    return weight;
}

float Soft() {
    static const float soft = [] {
        const char* v = std::getenv("VR_DLSS_FOCUS_SOFT");
        const float s = v ? std::strtof(v, nullptr) : DefaultSoft;
        return s >= 0.0f && s <= 1.0f ? s : DefaultSoft;
    }();
    return soft;
}

float Floor() {
    static const float floor = [] {
        const char* v = std::getenv("VR_DLSS_FOCUS_FLOOR");
        const float f = v ? std::strtof(v, nullptr) : DefaultFloor;
        return f >= 0.0f && f <= 32.0f ? f : DefaultFloor;
    }();
    return floor;
}

float Band() {
    static const float band = [] {
        const char* v = std::getenv("VR_DLSS_FOCUS_BAND");
        const float b = v ? std::strtof(v, nullptr) : DefaultBand;
        return b >= 1.0f && b <= 512.0f ? b : DefaultBand;
    }();
    return band;
}

bool EightBit(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

} // namespace

VrDlssFocusPass::VrDlssFocusPass(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()} {}

// The GPU may still run the last frames' passes: everything goes once it is done.
VrDlssFocusPass::~VrDlssFocusPass() {
    Release();
    scheduler.DeferOperation([s = std::move(sampler), cs = std::move(crop_set_layout),
                              ks = std::move(compose_set_layout), cl = std::move(crop_layout),
                              kl = std::move(compose_layout), c8 = std::move(crop_rgba8),
                              c16 = std::move(crop_rgba16f), k8 = std::move(compose_rgba8),
                              k16 = std::move(compose_rgba16f)]() mutable {
        c8.reset();
        c16.reset();
        k8.reset();
        k16.reset();
        cl.reset();
        kl.reset();
        cs.reset();
        ks.reset();
        s.reset();
    });
}

void VrDlssFocusPass::Release() {
    for (auto& t : targets) {
        for (Target* x : {&t.color, &t.depth, &t.motion, &t.output, &t.history}) {
            if (x->image)
                scheduler.DeferOperation(
                    [image = std::move(x->image), view = std::move(x->view)]() mutable {
                        view.reset();
                        image.Destroy();
                    });
            x->format = {};
            x->width = x->height = 0;
            x->fresh = false;
        }
        t.latest = VK_NULL_HANDLE;
        t.latest_frame = 0;
    }
}

void VrDlssFocusPass::Fail() {
    failed = true;
    Release(); // focus is off for the session: its images (about 100 MB) go
}

bool VrDlssFocusPass::Init() {
    tried = true;
    const auto fail = [&](std::string_view what) {
        LOG_ERROR(Render_Vulkan, "VR DLSS focus: {} failed; DLSS goes back to the whole eyes", what);
        failed = true;
        return false;
    };
    auto s = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    });
    if (s.result != vk::Result::eSuccess)
        return fail("sampler");
    sampler = std::move(s.value);
    const auto set_layout = [&](std::initializer_list<vk::DescriptorType> types,
                                vk::UniqueDescriptorSetLayout& out) {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for (const auto type : types)
            bindings.push_back({.binding = u32(bindings.size()),
                                .descriptorType = type,
                                .descriptorCount = 1,
                                .stageFlags = vk::ShaderStageFlagBits::eCompute});
        auto sl = device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        });
        if (sl.result != vk::Result::eSuccess)
            return false;
        out = std::move(sl.value);
        return true;
    };
    using T = vk::DescriptorType;
    if (!set_layout({T::eSampledImage, T::eSampledImage, T::eSampledImage, T::eStorageImage,
                     T::eStorageImage, T::eStorageImage},
                    crop_set_layout) ||
        !set_layout({T::eCombinedImageSampler, T::eStorageImage, T::eSampledImage,
                     T::eCombinedImageSampler, T::eSampledImage},
                    compose_set_layout))
        return fail("descriptor set layouts");
    const auto pipeline_layout = [&](vk::DescriptorSetLayout set, u32 push_size,
                                     vk::UniquePipelineLayout& out) {
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = push_size};
        auto pl = device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &set,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        });
        if (pl.result != vk::Result::eSuccess)
            return false;
        out = std::move(pl.value);
        return true;
    };
    if (!pipeline_layout(*crop_set_layout, sizeof(CropPush), crop_layout) ||
        !pipeline_layout(*compose_set_layout, sizeof(FocusPush), compose_layout))
        return fail("pipeline layouts");
    const auto make = [&](std::string_view code, vk::PipelineLayout layout,
                          std::vector<std::string> defines, const char* name,
                          vk::UniquePipeline& out) {
        const vk::ShaderModule module =
            Compile(code, vk::ShaderStageFlagBits::eCompute, device, std::move(defines));
        if (!module)
            return false;
        auto [result, p] = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage{.stage = vk::ShaderStageFlagBits::eCompute, .module = module,
                           .pName = "main"},
                    .layout = layout});
        device.destroyShaderModule(module);
        if (result != vk::Result::eSuccess)
            return false;
        out = std::move(p);
        SetObjectName(device, *out, std::string_view{name});
        return true;
    };
    if (!make(HostShaders::VR_DLSS_FOCUS_CROP_COMP, *crop_layout, {"CROP_RGBA8"},
              "vr_dlss_focus_crop.comp (rgba8)", crop_rgba8) ||
        !make(HostShaders::VR_DLSS_FOCUS_CROP_COMP, *crop_layout, {},
              "vr_dlss_focus_crop.comp (rgba16f)", crop_rgba16f) ||
        !make(HostShaders::VR_DLSS_FOCUS_COMP, *compose_layout, {"FOCUS_RGBA8"},
              "vr_dlss_focus.comp (rgba8)", compose_rgba8) ||
        !make(HostShaders::VR_DLSS_FOCUS_COMP, *compose_layout, {},
              "vr_dlss_focus.comp (rgba16f)", compose_rgba16f))
        return fail("pipelines");
    LOG_INFO(Render_Vulkan,
             "VR DLSS focus: ready (band {} eye px, jitter sign {}, TAA {:.2f}, soft {:.2f}, "
             "floor {:.1f})",
             Band(), JitterSign(), TaaWeight(), Soft(), Floor());
    return true;
}

bool VrDlssFocusPass::Ensure(Target& t, vk::Format format, u32 width, u32 height,
                             const char* name) {
    if (t.image && t.format == format && t.width == width && t.height == height)
        return true;
    if (t.image) {
        // The GPU may still read the old one (last frames' DLSS): freed once it is done.
        scheduler.DeferOperation([image = std::move(t.image), view = std::move(t.view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    t.format = {};
    t.width = t.height = 0;
    t.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    // TryCreate: out of memory (near the budget) turns focus off instead of ending the game.
    if (!t.image.TryCreate(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            // NGX may clear and copy its images with transfer commands (the output: a clear).
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        }))
        return false;
    SetObjectName(device, static_cast<vk::Image>(t.image), std::string_view{name});
    auto v = device.createImageViewUnique({
        .image = t.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    });
    if (v.result != vk::Result::eSuccess)
        return false;
    t.view = std::move(v.value);
    t.format = format;
    t.width = width;
    t.height = height;
    t.fresh = true;
    return true;
}

BbDlssImage VrDlssFocusPass::Image(const Target& t) {
    return BbDlssImage{static_cast<vk::Image>(t.image),
                       *t.view,
                       VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                       static_cast<VkFormat>(t.format),
                       t.width,
                       t.height};
}

bool VrDlssFocusPass::Crop(std::array<BbDlssEvaluate, 2>& eyes,
                           const std::array<DlssFocus::Rect, 2>& rects) {
    if (failed || (!tried && !Init()))
        return false;
    const auto refuse = [&](u32 eye, std::string_view why) {
        LOG_ERROR(Render_Vulkan, "VR DLSS focus: eye {} cannot be cut out ({})", eye, why);
        Fail();
        return false;
    };
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& e = eyes[eye];
        const auto& r = rects[eye];
        if (!r.On() || !e.color.view || !e.depth.view || !e.motion.view || !e.output.image)
            return refuse(eye, "missing input");
        if (r.in_x + r.in_w > e.color.width || r.in_y + r.in_h > e.color.height ||
            r.in_x + r.in_w > e.depth.width || r.in_y + r.in_h > e.depth.height ||
            r.in_x + r.in_w > e.motion.width || r.in_y + r.in_h > e.motion.height ||
            r.out_x + r.out_w > e.output.width || r.out_y + r.out_h > e.output.height)
            return refuse(eye, "rectangle outside the images");
        if (e.output.format != VK_FORMAT_R8G8B8A8_UNORM &&
            e.output.format != VK_FORMAT_R16G16B16A16_SFLOAT)
            return refuse(eye, "output format");
        auto& t = targets[eye];
        const char* names[2][4] = {
            {"vr_dlss_focus L colour", "vr_dlss_focus L depth", "vr_dlss_focus L motion",
             "vr_dlss_focus L output"},
            {"vr_dlss_focus R colour", "vr_dlss_focus R depth", "vr_dlss_focus R motion",
             "vr_dlss_focus R output"}};
        if (!Ensure(t.color,
                    EightBit(e.color.format) ? vk::Format::eR8G8B8A8Unorm
                                             : vk::Format::eR16G16B16A16Sfloat,
                    r.in_w, r.in_h, names[eye][0]) ||
            !Ensure(t.depth, vk::Format::eR32Sfloat, r.in_w, r.in_h, names[eye][1]) ||
            !Ensure(t.motion, vk::Format::eR16G16Sfloat, r.in_w, r.in_h, names[eye][2]) ||
            !Ensure(t.output, static_cast<vk::Format>(e.output.format), r.out_w, r.out_h,
                    names[eye][3]))
            return refuse(eye, "images");
    }
    GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0321ull, GpuPassProfiler::Group::Temporal,
                            "VR DLSS focus: the rectangles cut out for DLSS");
    const auto cmd = scheduler.Commands();
    // The inputs' writes (the scene, the motion pass) before these reads, and the last frame's
    // reads of the cut-outs (DLSS) before these writes; new images into GENERAL.
    std::array<vk::ImageMemoryBarrier2, 8> fresh{};
    u32 n = 0;
    for (auto& t : targets)
        for (Target* x : {&t.color, &t.depth, &t.motion, &t.output})
            if (x->fresh) {
                fresh[n++] = {.srcStageMask = vk::PipelineStageFlagBits2::eNone,
                              .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                              .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                               vk::AccessFlagBits2::eMemoryWrite,
                              .oldLayout = vk::ImageLayout::eUndefined,
                              .newLayout = vk::ImageLayout::eGeneral,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = static_cast<vk::Image>(x->image),
                              .subresourceRange = ColorRange};
                x->fresh = false;
            }
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &before,
                                            .imageMemoryBarrierCount = n,
                                            .pImageMemoryBarriers = fresh.data()});
    for (u32 eye = 0; eye < 2; ++eye) {
        auto& e = eyes[eye];
        const auto& r = rects[eye];
        auto& t = targets[eye];
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,
                         t.color.format == vk::Format::eR8G8B8A8Unorm ? *crop_rgba8
                                                                       : *crop_rgba16f);
        const std::array<vk::DescriptorImageInfo, 6> infos{{
            {.imageView = vk::ImageView{e.color.view}, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = vk::ImageView{e.depth.view}, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = vk::ImageView{e.motion.view}, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *t.color.view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *t.depth.view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *t.motion.view, .imageLayout = vk::ImageLayout::eGeneral},
        }};
        std::array<vk::WriteDescriptorSet, 6> writes{};
        for (u32 i = 0; i < writes.size(); ++i)
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i < 3 ? vk::DescriptorType::eSampledImage
                                                 : vk::DescriptorType::eStorageImage,
                         .pImageInfo = &infos[i]};
        const CropPush push{{s32(r.in_x), s32(r.in_y), s32(r.in_w), s32(r.in_h)}};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *crop_layout, 0, writes);
        ScratchPush::Invalidate();
        cmd.pushConstants(*crop_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmd.dispatch((r.in_w + Group - 1) / Group, (r.in_h + Group - 1) / Group, 1);
    }
    // The cut-outs before DLSS reads them (from any stage: compute or its own).
    const vk::MemoryBarrier2 cut{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &cut});
    for (u32 eye = 0; eye < 2; ++eye) {
        auto& e = eyes[eye];
        const auto& t = targets[eye];
        e.color = Image(t.color);
        e.depth = Image(t.depth);
        e.motion = Image(t.motion);
        e.output = Image(t.output);
    }
    return true;
}

bool VrDlssFocusPass::Compose(const std::array<Eye, 2>& eyes, u32 render_w, u32 render_h,
                              std::array<float, 2> jitter, bool reset, u64 frame,
                              std::array<BbDlssImage, 2>& written) {
    if (failed || !tried || !render_w || !render_h)
        return false;
    const auto pipeline_for = [&](VkFormat format) -> vk::Pipeline {
        return format == VK_FORMAT_R8G8B8A8_UNORM        ? *compose_rgba8
               : format == VK_FORMAT_R16G16B16A16_SFLOAT ? *compose_rgba16f
                                                         : vk::Pipeline{};
    };
    for (const auto& e : eyes) {
        if (!e.render || !e.motion || !e.dlss.view || !e.output.view || !e.rect.On() ||
            !pipeline_for(e.output.format) || e.dlss.width != e.rect.out_w ||
            e.dlss.height != e.rect.out_h || e.rect.out_x + e.rect.out_w > e.output.width ||
            e.rect.out_y + e.rect.out_h > e.output.height) {
            LOG_ERROR(Render_Vulkan, "VR DLSS focus: cannot compose an output {}x{} format {} "
                      "from {}x{} at ({}, {})", e.output.width, e.output.height,
                      int(e.output.format), e.dlss.width, e.dlss.height, e.rect.out_x,
                      e.rect.out_y);
            Fail();
            return false;
        }
    }
    // The TAA's turns: each eye's twin of its output (its size, 8 bits a channel whatever the
    // output's format: the render it enlarges is 8-bit; made again with the output's size, then
    // no history), the one written this frame the one not holding the last composed eye.
    std::array<vk::ImageMemoryBarrier2, 2> fresh{};
    u32 n = 0;
    std::array<BbDlssImage, 2> write{}, read{};
    std::array<bool, 2> history{};
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& e = eyes[eye];
        auto& t = targets[eye];
        if (!Ensure(t.history, vk::Format::eR8G8B8A8Unorm, e.output.width, e.output.height,
                    eye == 0 ? "vr_dlss_focus L history" : "vr_dlss_focus R history")) {
            LOG_ERROR(Render_Vulkan, "VR DLSS focus: the TAA's image could not be made");
            Fail();
            return false;
        }
        if (t.history.fresh) {
            fresh[n++] = {.srcStageMask = vk::PipelineStageFlagBits2::eNone,
                          .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                          .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                                           vk::AccessFlagBits2::eShaderStorageWrite,
                          .oldLayout = vk::ImageLayout::eUndefined,
                          .newLayout = vk::ImageLayout::eGeneral,
                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .image = static_cast<vk::Image>(t.history.image),
                          .subresourceRange = ColorRange};
            t.history.fresh = false;
            t.latest = VK_NULL_HANDLE;
        }
        const BbDlssImage twin = Image(t.history);
        const bool twin_next = t.latest == e.output.image; // the output holds the last eye
        write[eye] = twin_next ? twin : e.output;
        read[eye] = twin_next ? e.output : twin;
        // The last eye is history only if it is the last frame's (a handle alone could be a new
        // image's at an old one's address).
        history[eye] = !reset && frame != 0 && t.latest_frame + 1 == frame &&
                       (twin_next || t.latest == twin.image);
    }
    GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0320ull, GpuPassProfiler::Group::Temporal,
                            "VR DLSS focus: the eyes composed (DLSS inside, the render around)");
    const auto cmd = scheduler.Commands();
    // DLSS's writes before these reads; the last composed eyes' writes before they are read as
    // history; the images' last readers (the S1 copy) before these writes; new twins into GENERAL.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &before,
                                            .imageMemoryBarrierCount = n,
                                            .pImageMemoryBarriers = fresh.data()});
    const float sign = JitterSign();
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& e = eyes[eye];
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_for(write[eye].format));
        const std::array<vk::DescriptorImageInfo, 5> infos{{
            {.sampler = *sampler, .imageView = e.render, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = vk::ImageView{write[eye].view}, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = vk::ImageView{e.dlss.view}, .imageLayout = vk::ImageLayout::eGeneral},
            {.sampler = *sampler,
             .imageView = vk::ImageView{read[eye].view},
             .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = e.motion, .imageLayout = vk::ImageLayout::eGeneral},
        }};
        const std::array<vk::WriteDescriptorSet, 5> writes{{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
             .pImageInfo = &infos[0]},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &infos[1]},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &infos[2]},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
             .pImageInfo = &infos[3]},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &infos[4]},
        }};
        FocusPush push{};
        // A side on the eye's own edge has no seam to hide: far outside, so no band there.
        constexpr s32 Far = 1 << 20;
        const u32 x1 = e.rect.out_x + e.rect.out_w, y1 = e.rect.out_y + e.rect.out_h;
        push.rect[0] = e.rect.out_x == 0 ? -Far : s32(e.rect.out_x);
        push.rect[1] = e.rect.out_y == 0 ? -Far : s32(e.rect.out_y);
        push.rect[2] = x1 >= e.output.width ? Far : s32(x1);
        push.rect[3] = y1 >= e.output.height ? Far : s32(y1);
        push.map[0] = float(render_w) / float(e.output.width);
        push.map[1] = float(render_h) / float(e.output.height);
        push.map[2] = sign * jitter[0];
        push.map[3] = sign * jitter[1];
        push.inv[0] = 1.0f / float(render_w);
        push.inv[1] = 1.0f / float(render_h);
        push.inv[2] = Band();
        push.inv[3] = history[eye] ? TaaWeight() : 1.0f;
        push.size[0] = s32(e.output.width);
        push.size[1] = s32(e.output.height);
        push.size[2] = s32(render_w);
        push.size[3] = s32(render_h);
        push.frame[0] = s32(frame & 0x7fffffff);
        push.frame[1] = s32(Soft() * 1000.0f + 0.5f);
        push.frame[2] = s32(Floor() * 100.0f + 0.5f);
        push.origin[0] = s32(e.rect.out_x);
        push.origin[1] = s32(e.rect.out_y);
        push.origin[2] = s32(e.rect.out_w);
        push.origin[3] = s32(e.rect.out_h);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *compose_layout, 0, writes);
        ScratchPush::Invalidate();
        cmd.pushConstants(*compose_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                          &push);
        cmd.dispatch((e.output.width + Group - 1) / Group, (e.output.height + Group - 1) / Group,
                     1);
        targets[eye].latest = write[eye].image;
        targets[eye].latest_frame = frame;
        written[eye] = write[eye];
    }
    // The composed eyes before anything after them: the S1 copy that reads them, and the next
    // frame's writes.
    const vk::MemoryBarrier2 composed{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &composed});
    return true;
}

} // namespace Vulkan
