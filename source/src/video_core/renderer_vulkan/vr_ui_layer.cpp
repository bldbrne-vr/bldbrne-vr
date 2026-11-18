#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/vr/ui_conditions.h"
#include "core/vr/ui_lock_marker.h"
#include "core/vr/ui_slots.h"
#include "video_core/host_shaders/vr_ui_resolve_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_ui_layer.h"
#include "video_core/renderer_vulkan/vr_ui_snapshot.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

// FAST_GPU_UI_COPY (default on; '0': as before): the presenter's curved rest warp samples the
// atlas (OpenXRPresenter WarpUiRest's draw), so the atlases are also made sampled.
const bool g_atlas_sampled = [] {
    const char* value = std::getenv("FAST_GPU_UI_COPY");
    return value == nullptr || value[0] != '0';
}();

// Diagnostic A/B switch, read once at startup. Both layouts use the same resolve;
// compare against an unchanged build for an end-to-end performance claim.
const bool g_compact_snapshot = [] {
    const char* value = std::getenv("VR_UI_COMPACT_SNAPSHOT");
    return value == nullptr || value[0] != '0';
}();

/// Mirrors UiParams in vr_ui_resolve.comp (std430 push constants).
struct ResolvePush {
    s32 band[4];
    s32 cut[4][4];
    s32 box[4]; // the content box is measured in this band rect
    s32 scene_size[2];
    float grey;
    u32 flags;
    u32 cut_count;
    u32 raw_order; // the scene's raw channel i holds true component (raw_order >> 2i) & 3
    float gamma;   // the eyes' post-process gamma
    u32 split_px;  // min width (11 bits), gap, dilate, margin (7 bits each), band px
};
static_assert(offsetof(ResolvePush, cut) == 16);
static_assert(offsetof(ResolvePush, box) == 80);
static_assert(offsetof(ResolvePush, scene_size) == 96);
static_assert(sizeof(ResolvePush) == 128); // the guaranteed 128 bytes of push constants

enum ResolveFlag : u32 {
    FlagBgra = 1u << 0,
    FlagLinearSource = 1u << 1,
    FlagSrgbRt = 1u << 2,
    FlagVeil = 1u << 3,
    FlagGameplay = 1u << 4,
    FlagFullMenu = 1u << 5,
    FlagFootprint = 1u << 6,
    FlagMenuBox = 1u << 7,
    FlagMarker = 1u << 16,     // the lock-on marker's own pair (bindings 8, 9) ...
    FlagMarkerOver = 1u << 17, // ... over the rest of the UI (else under it)
    FlagEmpty = 1u << 18,      // pixels no UI drew on exactly transparent (ui_empty_snap)
    FlagDialogue = 1u << 22,
    FlagItemPickup = 1u << 21,
    FlagActionPrompt = 1u << 20, // action prompt region belongs to the HUD throughout its fade
    FlagConditions = 1u << 19, // cut 3 is the build-up meter: into the strip (ui_conditions)
    FlagAreaName = 1u << 23,   // an area name is up: its corner whole on the band
    FlagAreaKey = 1u << 24,    // ... and its dark backing keyed out
    FlagSheet = 1u << 25,      // the slot sheet below the halves (mode 9), no strip
};
constexpr u32 StatusShift = 8;   // flags bits 8..11: cuts left out of the content box
constexpr u32 DynamicShift = 12; // flags bits 12..15: the dynamic cut

enum Mode : u32 {
    ModeVeil = 0,
    ModeAtlas = 1,
    ModeSceneVeil = 2,
    ModeCheck = 3,
    ModeSplitRows = 4,
    ModeSplit = 5,
    ModeMenuGone = 6,
    ModeMenuHud = 7,
    ModeMarkerBox = 8,
    ModeSlots = 9,
    ModeVeilOriginal = 10, // mode 0 as it was (VR_UI_RESOLVE_FAST=0)
    ModeVeilCheck = 11,    // ... comparing its words with mode 0's (VR_UI_RESOLVE_FAST=v)
    ModeSplitRowsCheck = 12, // mode 4, also walking every row the original way (=v)
};

// VR_UI_RESOLVE_FAST: the one-workgroup passes reading ahead (the veil estimate, mode 0, and the
// split rows, mode 4: the same sums in the same order) and the scene veil dispatched only with a
// veil (its groups from mode 0's words, an indirect dispatch), on by default; '0' the original
// veil estimate and scene veil; 'v' on, with mode 11 after mode 0 and mode 12 for mode 4 every
// frame (any word or row that differs from the original passes is counted and logged).
char ResolveFast() {
    static const char mode = [] {
        const char* value = std::getenv("VR_UI_RESOLVE_FAST");
        if (value == nullptr) {
            return '1';
        }
        return value[0] == '0' ? '0' : value[0] == 'v' ? 'v' : '1';
    }();
    return mode;
}

// Stats words (vr_ui_resolve.comp).
constexpr u32 WVeil = 0;
constexpr u32 WVeilOn = 1;
constexpr u32 WStripT = 2;
constexpr u32 WStripD = 3;
constexpr u32 WStripK = 4;
constexpr u32 WContent = 5;
constexpr u32 WRest = 6;
constexpr u32 WBorder = 7;
constexpr u32 WCut0 = 8;
constexpr u32 WOpaque = 12;
constexpr u32 WSplitRight = 13;
constexpr u32 WCheckN = 16;
constexpr u32 WCheckBad = 17;
constexpr u32 WCheckMax = 18;
constexpr u32 WCheckSum = 19;
constexpr u32 WCheckBad8 = 20;
constexpr u32 WSaturated = 21;
constexpr u32 WSplitRows = 22;
constexpr u32 WSplitOut = 23;
constexpr u32 WMenuGone = 24;
constexpr u32 WMenuHud = 25;
constexpr u32 WMenuHudOut = 26;
constexpr u32 WMenuHudVeil = 27;
constexpr u32 WOpaqueBox = 28; // the opaque UI's box (28..31)
constexpr u32 WVeilT = 32;
constexpr u32 WVeilK = 35;
constexpr u32 WStripKd = 38;
constexpr u32 WBox = 40;
constexpr u32 WCutBox = 44; // 4 words per cut-out (44..59)
constexpr u32 WMarkBox = 60; // the lock-on marker's box (60..63)
constexpr u32 WSlotArea = 64;   // mode 9: UI pixels in the area name's slot ...
constexpr u32 WSlotBanner = 65; // ... and in the banners' rows
constexpr u32 WVerifyN = 66;    // mode 11: 1 when it compared this frame
constexpr u32 WVerifyBad = 67;  // ... 1 when mode 0's words differed
constexpr u32 WVeilArgs = 68;   // the scene veil's groups x, y, z (68..70)
constexpr u32 WVerifySplit = 71; // mode 12: split rows that differed

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceLayers ColorLayers{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

vk::Format SrgbTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eB8G8R8A8Unorm:
        return vk::Format::eB8G8R8A8Srgb;
    case vk::Format::eA8B8G8R8UnormPack32:
        return vk::Format::eA8B8G8R8SrgbPack32;
    default:
        return vk::Format::eUndefined;
    }
}

vk::Format UnormTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eA8B8G8R8SrgbPack32:
        return vk::Format::eA8B8G8R8UnormPack32;
    default:
        return format;
    }
}

bool IsSrgb(vk::Format format) {
    return UnormTwin(format) != format;
}

bool IsFloat(vk::Format format) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eE5B9G9R9UfloatPack32:
        return true;
    default:
        return false;
    }
}

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

// The image channel component `c` of a component mapping reads (identity: its own).
s32 SourceChannel(vk::ComponentSwizzle swizzle, s32 own) {
    switch (swizzle) {
    case vk::ComponentSwizzle::eIdentity:
        return own;
    case vk::ComponentSwizzle::eR:
        return 0;
    case vk::ComponentSwizzle::eG:
        return 1;
    case vk::ComponentSwizzle::eB:
        return 2;
    case vk::ComponentSwizzle::eA:
        return 3;
    default:
        return -1;
    }
}

// Packed raw -> true component order of a mapping (2 bits per raw channel; 3 = alpha / none).
u32 RawOrder(const vk::ComponentMapping& m) {
    std::array<u32, 4> order{3, 3, 3, 3};
    const std::array<vk::ComponentSwizzle, 3> rgb{m.r, m.g, m.b};
    for (u32 c = 0; c < 3; ++c) {
        const s32 raw = SourceChannel(rgb[c], static_cast<s32>(c));
        if (raw >= 0) {
            order[raw] = c;
        }
    }
    return order[0] | order[1] << 2 | order[2] << 4 | order[3] << 6;
}

// Which channel of the image the true alpha is read from (component mapping), -1 = none.
s32 AlphaChannel(const vk::ComponentMapping& m) {
    switch (m.a) {
    case vk::ComponentSwizzle::eIdentity:
    case vk::ComponentSwizzle::eA:
        return 3;
    case vk::ComponentSwizzle::eR:
        return 0;
    case vk::ComponentSwizzle::eG:
        return 1;
    case vk::ComponentSwizzle::eB:
        return 2;
    default:
        return -1;
    }
}

template<class Command>
void ImageBarrier(Command cmdbuf, vk::Image image, vk::ImageLayout from,
                  vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    auto range = ColorRange;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
}

void MemoryBarrier(vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                   vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

} // Anonymous namespace

VrUiLayer::VrUiLayer(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()},
      allocator{instance_.GetAllocator()} {
    // 0, 1: K and W (sampled); 2: atlas (storage); 3: stats (buffer); 4: the scene copy
    // (storage, the veil); 5, 6: X and the scene copy (sampled, the check); 7: the split (buffer);
    // 8, 9: the lock-on marker's own K and W (sampled).
    std::array<vk::DescriptorSetLayoutBinding, 10> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i == 2 || i == 4   ? vk::DescriptorType::eStorageImage
                              : i == 3 || i == 7 ? vk::DescriptorType::eStorageBuffer
                                                 : vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    set_layout =
        Check<"create vr ui descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(ResolvePush),
    };
    pipeline_layout = Check<"create vr ui pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    SetObjectName(device, *pipeline_layout, "vr ui pipeline layout");
    // One source, one pipeline per MODE (defined right after the #version line).
    const std::string source{HostShaders::VR_UI_RESOLVE_COMP};
    const size_t version = source.find("\n#version"); // not the comments' mention of it
    ASSERT(version != std::string::npos);
    const size_t line_end = source.find('\n', version + 1);
    static constexpr std::array<const char*, 13> names{
        "vr ui veil",       "vr ui atlas",          "vr ui scene veil", "vr ui check",
        "vr ui split rows", "vr ui split",          "vr ui menu gone",  "vr ui menu hud",
        "vr ui marker box", "vr ui slot sheet",     "vr ui veil (original)",
        "vr ui veil check", "vr ui split rows check"};
    for (u32 mode = 0; mode < pipelines.size(); ++mode) {
        // The original veil estimate and the checks only where VR_UI_RESOLVE_FAST uses them.
        if ((mode == ModeVeilOriginal && ResolveFast() != '0') ||
            ((mode == ModeVeilCheck || mode == ModeSplitRowsCheck) && ResolveFast() != 'v')) {
            continue;
        }
        std::string code = source;
        code.insert(line_end + 1, fmt::format("#define MODE {}\n", mode));
        const vk::ShaderModule module = Compile(code, vk::ShaderStageFlagBits::eCompute, device);
        ASSERT_MSG(module, "Failed to compile vr_ui_resolve.comp (mode {})", mode);
        const vk::ComputePipelineCreateInfo pipeline_ci{
            .stage{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = *pipeline_layout,
        };
        pipelines[mode] = Check<"create vr ui compute pipeline">(
            device.createComputePipelineUnique({}, pipeline_ci));
        SetObjectName(device, *pipelines[mode], std::string_view{names[mode]});
        device.destroyShaderModule(module);
    }

    // The stats words (device) and their per-frame host copies.
    {
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = StatsWords * sizeof(u32),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        };
        const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer buffer{};
        ASSERT_MSG(vmaCreateBuffer(allocator, &info, &alloc, &buffer, &stats_allocation, nullptr) ==
                       VK_SUCCESS,
                   "Failed to allocate the VR UI stats buffer");
        stats_buffer = vk::Buffer{buffer};
        SetObjectName(device, stats_buffer, "VR UI stats");
    }
    {
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = ReadbackSlots * StatsWords * sizeof(u32),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc{
            .flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VkBuffer buffer{};
        VmaAllocationInfo alloc_info{};
        ASSERT_MSG(vmaCreateBuffer(allocator, &info, &alloc, &buffer, &readback_allocation,
                                   &alloc_info) == VK_SUCCESS,
                   "Failed to allocate the VR UI readback buffer");
        readback_buffer = vk::Buffer{buffer};
        readback_words = static_cast<const u32*>(alloc_info.pMappedData);
        SetObjectName(device, readback_buffer, "VR UI stats readback");
    }
    {
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = SplitBytes,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer buffer{};
        ASSERT_MSG(vmaCreateBuffer(allocator, &info, &alloc, &buffer, &split_allocation, nullptr) ==
                       VK_SUCCESS,
                   "Failed to allocate the VR UI split buffer");
        split_buffer = vk::Buffer{buffer};
        SetObjectName(device, split_buffer, "VR UI split");
    }
}

// The owner makes sure the GPU no longer uses anything (WaitIdle).
VrUiLayer::~VrUiLayer() {
    if (stats_buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(stats_buffer), stats_allocation);
    }
    if (readback_buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(readback_buffer), readback_allocation);
    }
    if (split_buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(split_buffer), split_allocation);
    }
}

void VrUiLayer::Retire(VideoCore::UniqueImage& image, std::vector<vk::UniqueImageView> views) {
    if (!image) {
        return;
    }
    // Commands recorded up to the current tick may still use it.
    scheduler.DeferOperation([image = std::move(image), views = std::move(views)]() mutable {
        views.clear();
        image.Destroy();
    });
}

void VrUiLayer::EnsureTargets(vk::Format x_format, vk::Extent2D x_extent,
                              const vk::ComponentMapping& comps, u32 layers) {
    if (targets[0].image && format == x_format && extent == x_extent && components == comps &&
        target_layers == layers) {
        return;
    }
    for (auto& target : targets) {
        std::vector<vk::UniqueImageView> views;
        views.push_back(std::move(target.sampled));
        for (auto& [view_format, view] : target.rt_views) {
            views.push_back(std::move(view));
        }
        target.rt_views.clear();
        Retire(target.image, std::move(views));
        target.layout = vk::ImageLayout::eUndefined;
    }
    for (auto& keep : hud_keep) {
        std::vector<vk::UniqueImageView> views;
        views.push_back(std::move(keep.sampled));
        Retire(keep.image, std::move(views));
    }
    format = x_format;
    extent = x_extent;
    components = comps;
    target_layers = layers;
    hud_keep_extent = {};
    hud_keep_rect = {};
    for (u32 i = 0; i < targets.size(); ++i) {
        if (i == MarkBlack || i == MarkGrey) {
            continue; // made at the marker's first draw (TargetView)
        }
        CreateTarget(i);
    }
    // No snapshot with the HUD in its slots (ui_hud_slots): the menu never draws over it there.
    if (VR::Ui::Slots::Active() == nullptr) {
        const auto rows = UiSnapshot::MenuRows(extent.width, extent.height);
        EnsureHudKeep(g_compact_snapshot ? vk::Extent2D{rows.Width(), rows.Height()} : extent);
    }
    LOG_INFO(Render_Vulkan,
             "VR ui layer: K/W targets {}x{} {} read as {}{}{}{}; menu HUD snapshots allocated "
             "in {} layout, grey {:.2f}",
             x_extent.width, x_extent.height, vk::to_string(x_format), vk::to_string(comps.r),
             vk::to_string(comps.g), vk::to_string(comps.b), vk::to_string(comps.a),
             g_compact_snapshot ? "compact" : "full-size diagnostic", grey);
}

void VrUiLayer::CreateTarget(u32 i) {
    auto& target = targets[i];
    const vk::ImageCreateInfo image_ci{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent{.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = target_layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    target.image = VideoCore::UniqueImage(device, allocator);
    target.image.Create(image_ci);
    static constexpr std::array<const char*, 4> target_names{"K", "W", "marker K", "marker W"};
    SetObjectName(device, static_cast<vk::Image>(target.image), "VR UI {} {}x{}", target_names[i],
                  extent.width, extent.height);
    target.sampled = Check<"create vr ui target view">(device.createImageViewUnique({
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = UnormTwin(format),
        .components = components,
        .subresourceRange = ColorRange,
    }));
    target.layout = vk::ImageLayout::eUndefined;
}

bool VrUiLayer::BeginFrame(vk::Format x_format, vk::Extent2D x_extent,
                           const vk::ComponentMapping& comps, float grey_value, u32 layers) {
    frame_active = false;
    written = false;
    marker_written = false;
    srgb_rt = false;
    hud_snap = HudSnap::Whole;
    for (auto& target : targets) {
        target.cleared = false;
    }
    if (x_format == vk::Format::eUndefined || x_extent.width == 0 || x_extent.height == 0 ||
        !instance.IsFormatSupported(x_format,
                                    vk::FormatFeatureFlagBits2::eColorAttachment |
                                        vk::FormatFeatureFlagBits2::eColorAttachmentBlend |
                                        vk::FormatFeatureFlagBits2::eSampledImage)) {
        return false;
    }
    grey = std::clamp(grey_value, 0.25f, 0.9f);
    EnsureTargets(x_format, x_extent, comps, layers);
    frame_active = true;
    return true;
}

vk::ImageView VrUiLayer::TargetView(RecorderProxy cmdbuf, Which which, vk::Format view_format) {
    auto& target = targets[which];
    if (frame_active && !target.image && (which == MarkBlack || which == MarkGrey) &&
        targets[Black].image) {
        CreateTarget(which); // the lock-on marker's first draw (outside a render pass)
    }
    if (!frame_active || !target.image) {
        return {};
    }
    vk::ImageView view{};
    for (const auto& [f, v] : target.rt_views) {
        if (f == view_format) {
            view = *v;
        }
    }
    if (!view) {
        auto range = ColorRange;
        range.layerCount = target_layers;
        auto unique = Check<"create vr ui render target view">(device.createImageViewUnique({
            .image = target.image,
            .viewType = target_layers == 2 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
            .format = view_format,
            .subresourceRange = range,
        }));
        view = *unique;
        target.rt_views.emplace_back(view_format, std::move(unique));
    }
    if (target.layout != vk::ImageLayout::eColorAttachmentOptimal) {
        // The first pass of the frame clears it: the previous frame's resolve only has to have
        // read it (write-after-read).
        ImageBarrier(
            cmdbuf, target.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead);
        target.layout = vk::ImageLayout::eColorAttachmentOptimal;
        target.cleared = false;
    }
    if (IsSrgb(view_format)) {
        srgb_rt = true;
    }
    written = true;
    if (which == MarkBlack || which == MarkGrey) {
        marker_written = true;
    }
    return view;
}

bool VrUiLayer::TakeClear(Which which, std::array<u32, 4>& bits) {
    auto& target = targets[which];
    if (target.cleared) {
        return false;
    }
    target.cleared = true;
    const float value = which == Black || which == MarkBlack ? 0.0f : grey;
    const s32 alpha = AlphaChannel(components);
    for (s32 c = 0; c < 4; ++c) {
        bits[c] = std::bit_cast<u32>(c == alpha ? 0.0f : value);
    }
    return true;
}

vk::ClearColorValue VrUiLayer::ClearColor(Which which) const {
    const float value = which == Black || which == MarkBlack ? 0.0f : grey;
    const s32 alpha = AlphaChannel(components);
    vk::ClearColorValue clear{};
    for (s32 c = 0; c < 4; ++c) {
        clear.float32[c] = c == alpha ? 0.0f : value;
    }
    return clear;
}

void VrUiLayer::NoteGameClear(Which which) {
    targets[which].cleared = true;
}

void VrUiLayer::EnsureHudKeep(vk::Extent2D required) {
    // Exact texels, no unused full-screen rows. Retain capacity until capture targets
    // change; Retire keeps replaced images alive for earlier GPU commands.
    if (required.width > hud_keep_extent.width || required.height > hud_keep_extent.height) {
        hud_keep_extent.width = std::max(hud_keep_extent.width, required.width);
        hud_keep_extent.height = std::max(hud_keep_extent.height, required.height);
        for (const auto which : {Black, Grey}) {
            auto& keep = hud_keep[which];
            std::vector<vk::UniqueImageView> views;
            views.push_back(std::move(keep.sampled));
            Retire(keep.image, std::move(views));
            auto ci = targets[which].image.image_ci;
            ci.extent = vk::Extent3D{hud_keep_extent.width, hud_keep_extent.height, 1};
            ci.arrayLayers = 1; // the common UI from view zero
            ci.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
            keep.image = VideoCore::UniqueImage(device, allocator);
            keep.image.Create(ci);
            keep.sampled = Check<"create vr ui kept view">(device.createImageViewUnique({
                .image = keep.image,
                .viewType = vk::ImageViewType::e2D,
                .format = UnormTwin(format),
                .components = components,
                .subresourceRange = ColorRange,
            }));
            SetObjectName(device, static_cast<vk::Image>(keep.image), "VR UI {} kept {}x{}",
                          which == Black ? "K" : "W", hud_keep_extent.width,
                          hud_keep_extent.height);
        }
        LOG_INFO(Render_Vulkan, "VR ui: menu HUD snapshot pair {}x{} (source {}x{}, no resampling)",
                 hud_keep_extent.width, hud_keep_extent.height, extent.width, extent.height);
    }
}

void VrUiLayer::SnapshotHud(vk::CommandBuffer cmdbuf, const vk::Rect2D& rows) {
    if (!frame_active || hud_snap != HudSnap::Whole) {
        return;
    }
    // No UI draw yet (K and W not even cleared for this frame): the menu came first.
    if (!written || !targets[Black].cleared || !targets[Grey].cleared) {
        hud_snap = HudSnap::Empty;
        return;
    }
    const auto r = UiSnapshot::Clip(extent.width, extent.height, rows.offset.x, rows.offset.y,
                                    rows.extent.width, rows.extent.height);
    if (r.Empty()) {
        hud_snap = HudSnap::Empty;
        return;
    }
    const vk::Extent2D copy_extent{r.Width(), r.Height()};
    EnsureHudKeep(g_compact_snapshot ? copy_extent : extent);
    hud_keep_rect = g_compact_snapshot ? vk::Rect2D{{r.x0, r.y0}, copy_extent}
                                       : vk::Rect2D{{0, 0}, extent};
    const vk::ImageCopy region{
        .srcSubresource = ColorLayers,
        .srcOffset = {r.x0, r.y0, 0},
        .dstSubresource = ColorLayers,
        .dstOffset = g_compact_snapshot ? vk::Offset3D{0, 0, 0} : vk::Offset3D{r.x0, r.y0, 0},
        .extent = {.width = copy_extent.width, .height = copy_extent.height, .depth = 1},
    };
    for (const auto which : {Black, Grey}) {
        auto& target = targets[which];
        auto& keep = hud_keep[which];
        // K / W: the frame's UI passes so far are done; they go on after the copy.
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eColorAttachmentOptimal,
                     vk::ImageLayout::eTransferSrcOptimal,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        // The copy: the previous frame's resolve may still read it (write-after-read); the rows
        // outside the region are never read.
        ImageBarrier(cmdbuf, keep.image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(target.image, vk::ImageLayout::eTransferSrcOptimal, keep.image,
                         vk::ImageLayout::eTransferDstOptimal, region);
        ImageBarrier(cmdbuf, keep.image, vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderSampledRead);
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eTransferSrcOptimal,
                     vk::ImageLayout::eColorAttachmentOptimal,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite);
    }
    hud_snap = HudSnap::Kept;
}

bool VrUiLayer::CaptureScene(const RecorderProxy& cmdbuf, vk::Image x, vk::ImageLayout layout,
                             vk::Format x_format, vk::Extent2D x_extent,
                             const vk::ComponentMapping& comps, u32 layers) {
    scene_valid = false;
    if (!x || x_format == vk::Format::eUndefined || x_extent.width == 0 || x_extent.height == 0) {
        return false;
    }
    if (!scene.image || scene.extent != x_extent || scene.format != x_format ||
        scene.components != comps || scene.layers != layers) {
        std::vector<vk::UniqueImageView> views;
        views.push_back(std::move(scene.sampled));
        views.push_back(std::move(scene.storage));
        views.push_back(std::move(scene.decoded));
        views.push_back(std::move(scene.right_storage));
        for (auto& entry : scene.native_views) {
            for (auto& view : entry.second) {
                views.push_back(std::move(view));
            }
        }
        scene.native_views.clear();
        Retire(scene.image, std::move(views));
        const vk::Format raw = UnormTwin(x_format);
        const vk::Format srgb = SrgbTwin(raw);
        const bool storage =
            instance.IsFormatSupported(raw, vk::FormatFeatureFlagBits2::eStorageImage);
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst |
                                    vk::ImageUsageFlagBits::eTransferSrc |
                                    vk::ImageUsageFlagBits::eSampled;
        if (storage) {
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
        const vk::ImageCreateInfo image_ci{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = x_format,
            .extent{.width = x_extent.width, .height = x_extent.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        scene.image = VideoCore::UniqueImage(device, allocator);
        scene.image.Create(image_ci);
        SetObjectName(device, static_cast<vk::Image>(scene.image), "VR UI scene {}x{}",
                      x_extent.width, x_extent.height);
        // Only the storage view may be used as storage (an sRGB view cannot be).
        const vk::ImageViewUsageCreateInfo sampled_usage{.usage = vk::ImageUsageFlagBits::eSampled};
        scene.sampled = Check<"create vr ui scene view">(device.createImageViewUnique({
            .pNext = &sampled_usage,
            .image = scene.image,
            .viewType = vk::ImageViewType::e2D,
            .format = raw,
            .components = comps,
            .subresourceRange = ColorRange,
        }));
        if (storage) {
            const vk::ImageViewUsageCreateInfo storage_usage{.usage =
                                                                 vk::ImageUsageFlagBits::eStorage};
            scene.storage = Check<"create vr ui scene storage view">(device.createImageViewUnique({
                .pNext = &storage_usage,
                .image = scene.image,
                .viewType = vk::ImageViewType::e2D,
                .format = raw,
                .subresourceRange = ColorRange,
            }));
        }
        if (srgb != vk::Format::eUndefined) {
            scene.decoded = Check<"create vr ui scene srgb view">(device.createImageViewUnique({
                .pNext = &sampled_usage,
                .image = scene.image,
                .viewType = vk::ImageViewType::e2D,
                .format = srgb,
                .components = comps,
                .subresourceRange = ColorRange,
            }));
        }
        if (storage && layers == 2) {
            auto range = ColorRange;
            range.baseArrayLayer = 1;
            const vk::ImageViewUsageCreateInfo storage_usage{.usage = vk::ImageUsageFlagBits::eStorage};
            scene.right_storage = Check<"create vr ui right scene storage view">(
                device.createImageViewUnique({
                    .pNext = &storage_usage,
                    .image = scene.image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = raw,
                    .subresourceRange = range,
                }));
        }
        scene.layers = layers;
        scene.extent = x_extent;
        scene.format = x_format;
        scene.components = comps;
    }
    // The previous frame's eye pass, veil and check may still read or write it.
    ImageBarrier(
        cmdbuf, scene.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader |
            vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite);
    auto copy_layers = ColorLayers;
    copy_layers.layerCount = layers;
    const vk::ImageCopy region{
        .srcSubresource = copy_layers,
        .srcOffset = {},
        .dstSubresource = copy_layers,
        .dstOffset = {},
        .extent = {.width = x_extent.width, .height = x_extent.height, .depth = 1},
    };
    cmdbuf.copyImage(x, layout, scene.image, vk::ImageLayout::eTransferDstOptimal, region);
    ImageBarrier(cmdbuf, scene.image, vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                 vk::AccessFlagBits2::eTransferWrite,
                 vk::PipelineStageFlagBits2::eComputeShader |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eShaderSampledRead);
    scene_valid = true;
    return true;
}

std::array<vk::ImageView, 2> VrUiLayer::NativeSceneViews(vk::Format view_format) {
    ASSERT(scene_valid && scene.layers == 2);
    for (const auto& entry : scene.native_views) {
        if (entry.first == view_format) {
            return {*entry.second[0], *entry.second[1]};
        }
    }
    std::array<vk::UniqueImageView, 2> views;
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    for (u32 eye = 0; eye < 2; ++eye) {
        views[eye] = Check<"create native scene view">(device.createImageViewUnique({
            .pNext = &usage,
            .image = scene.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, eye, 1},
        }));
    }
    const std::array<vk::ImageView, 2> result{*views[0], *views[1]};
    scene.native_views.emplace_back(view_format, std::move(views));
    return result;
}

VrUiLayer::SceneRef VrUiLayer::Scene() const {
    if (!scene_valid) {
        return {};
    }
    return {
        .image = scene.image,
        .sampled = *scene.sampled,
        .storage = scene.storage ? *scene.storage : vk::ImageView{},
        .decoded = scene.decoded ? *scene.decoded : vk::ImageView{},
        .extent = scene.extent,
        .layout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .layers = scene.layers,
        .right_storage = scene.right_storage ? *scene.right_storage : vk::ImageView{},
    };
}

void VrUiLayer::EndFrame() {
    if (query_pool && timing_written != 0) {
        // The frame's spans are complete only when all three timestamps were written.
        timing_pending[timing_slot] = timing_written == 0x7;
        timing_slot = (timing_slot + 1) % TimingSlots;
        timing_written = 0;
    }
    frame_active = false;
    written = false;
    marker_written = false;
    scene_valid = false;
}

u32 VrUiLayer::AtlasRows(vk::Extent2D band, bool strip, u32 sheet_rows) {
    // The slot sheet where the strip would be, padded to a multiple of 8 like it (the mips).
    return sheet_rows > 0
               ? (VR::Ui::Conditions::StripTop(band.height) + sheet_rows + 7u) & ~7u
               : VR::Ui::Conditions::AtlasHeight(band.width, band.height, strip);
}

VrUiLayer::Atlas VrUiLayer::AtlasFor(u32 slot, vk::Extent2D band, bool strip, u32 sheet_rows) {
    if (atlases.size() <= slot) {
        atlases.resize(slot + 1);
    }
    auto& atlas = atlases[slot];
    const vk::Extent2D size{band.width, AtlasRows(band, strip, sheet_rows)};
    if (!atlas.image || atlas.extent != size) {
        std::vector<vk::UniqueImageView> views;
        views.push_back(std::move(atlas.storage));
        Retire(atlas.image, std::move(views));
        const vk::ImageCreateInfo image_ci{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent{.width = size.width, .height = size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
                     (g_atlas_sampled ? vk::ImageUsageFlagBits::eSampled : vk::ImageUsageFlags{}),
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        atlas.image = VideoCore::UniqueImage(device, allocator);
        atlas.image.Create(image_ci);
        SetObjectName(device, static_cast<vk::Image>(atlas.image), "VR UI atlas {} {}x{}", slot,
                      size.width, size.height);
        atlas.storage = Check<"create vr ui atlas view">(device.createImageViewUnique({
            .image = atlas.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = ColorRange,
        }));
        atlas.extent = size;
    }
    return {atlas.image, *atlas.storage, atlas.extent};
}

VrUiLayer::EyeImage VrUiLayer::EyeImageFor(u32 slot, vk::Format eye_format, vk::Extent2D size) {
    if (eye_images.size() <= slot) {
        eye_images.resize(slot + 1);
    }
    auto& eye = eye_images[slot];
    if (!eye.image || eye.extent != size || eye.format != eye_format) {
        std::vector<vk::UniqueImageView> views;
        views.push_back(std::move(eye.view));
        Retire(eye.image, std::move(views));
        const vk::ImageCreateInfo image_ci{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = eye_format,
            .extent{.width = size.width, .height = size.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        eye.image = VideoCore::UniqueImage(device, allocator);
        eye.image.Create(image_ci);
        SetObjectName(device, static_cast<vk::Image>(eye.image), "VR UI mono eye {} {}x{}", slot,
                      size.width, size.height);
        eye.view = Check<"create vr ui eye view">(device.createImageViewUnique({
            .image = eye.image,
            .viewType = vk::ImageViewType::e2D,
            .format = eye_format,
            .subresourceRange = ColorRange,
        }));
        eye.extent = size;
        eye.format = eye_format;
    }
    return {eye.image, *eye.view, eye.extent};
}

bool VrUiLayer::Resolve(vk::CommandBuffer cmdbuf, const Atlas* atlas, const SceneRef& scene_ref,
                        const ResolveParams& params) {
    if (!frame_active || !written || params.band.extent.width == 0 ||
        params.band.extent.height == 0 ||
        params.band.offset.x + params.band.extent.width > extent.width ||
        params.band.offset.y + params.band.extent.height > extent.height) {
        return false;
    }
    if (!params.check &&
        (atlas == nullptr || !atlas->image || atlas->extent.width != params.band.extent.width ||
         atlas->extent.height != AtlasRows(params.band.extent, params.strip, 0u))) {
        return false;
    }
    if (params.check && (!params.x_final.sampled || !scene_ref.sampled)) {
        return false;
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/VR UI resolve"});
    }
    if ((timing_written & 0x1) != 0) {
        Timestamp(cmdbuf, 1);
    }
    const bool marker = marker_written && !params.check;
    for (const auto which : {Black, Grey}) {
        auto& target = targets[which];
        if (target.layout == vk::ImageLayout::eColorAttachmentOptimal && target.cleared) {
            continue;
        }
        // Only the lock-on marker drew this frame (its own pair): K and W are this frame's empty
        // UI, not the last frame's.
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eComputeShader |
                         vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                     vk::AccessFlagBits2::eTransferWrite);
        auto clear_range = ColorRange;
        clear_range.layerCount = target_layers;
        cmdbuf.clearColorImage(target.image, vk::ImageLayout::eTransferDstOptimal,
                               ClearColor(which), clear_range);
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eTransfer,
                     vk::AccessFlagBits2::eTransferWrite,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite);
        target.layout = vk::ImageLayout::eColorAttachmentOptimal;
        target.cleared = true;
    }
    // K and W (and the marker's pair): the frame's passes are done; the resolve reads them.
    for (const auto which : {Black, Grey, MarkBlack, MarkGrey}) {
        auto& target = targets[which];
        if ((which == MarkBlack || which == MarkGrey) && !marker) {
            continue;
        }
        ImageBarrier(cmdbuf, target.image, target.layout, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderSampledRead);
        target.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    // The stats words: the previous frame's passes, scene veil dispatch and copy-out are done with
    // them.
    MemoryBarrier(
        cmdbuf,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer |
            vk::PipelineStageFlagBits2::eDrawIndirect,
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eTransferRead,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead);

    ResolvePush push{};
    push.band[0] = params.band.offset.x;
    push.band[1] = params.band.offset.y;
    push.band[2] = static_cast<s32>(params.band.extent.width);
    push.band[3] = static_cast<s32>(params.band.extent.height);
    push.cut_count = std::min<u32>(params.cut_count, 4);
    for (u32 i = 0; i < push.cut_count; ++i) {
        push.cut[i][0] = params.cut[i].x0;
        push.cut[i][1] = params.cut[i].y0;
        push.cut[i][2] = params.cut[i].x1;
        push.cut[i][3] = params.cut[i].y1;
    }
    push.box[0] = params.box.x0;
    push.box[1] = params.box.y0;
    push.box[2] = params.box.x1;
    push.box[3] = params.box.y1;
    push.scene_size[0] = static_cast<s32>(scene_ref.extent.width);
    push.scene_size[1] = static_cast<s32>(scene_ref.extent.height);
    push.grey = grey;
    // The dynamic cut is split only in a gameplay frame whose mask fits the split buffer.
    s32 dynamic = params.dynamic_cut;
    if (dynamic >= 0 && static_cast<u32>(dynamic) < push.cut_count) {
        const Region& r = params.cut[dynamic];
        const u32 rows = static_cast<u32>(std::max(r.y1 - r.y0, 0));
        const u32 words = DivCeil(static_cast<u32>(std::max(r.x1 - r.x0, 0)), 32);
        if (rows == 0 || words == 0 || rows > 1024 ||
            (2048 + rows * words) * sizeof(u32) > SplitBytes) {
            dynamic = -1;
        }
    } else {
        dynamic = -1;
    }
    const bool split = !params.check && params.gameplay && dynamic >= 0;
    const bool menu_hud = !params.check && params.menu_hud && params.status_cuts != 0 &&
                          hud_snap != HudSnap::Empty;
    push.flags = (params.bgra ? FlagBgra : 0u) | (IsFloat(format) ? FlagLinearSource : 0u) |
                 (srgb_rt ? FlagSrgbRt : 0u) | (params.veil ? FlagVeil : 0u) |
                 (split ? FlagGameplay : 0u) | (params.full_menu ? FlagFullMenu : 0u) |
                 (split && params.dialogue ? FlagDialogue : 0u) |
                 (split && params.item_pickup ? FlagItemPickup : 0u) |
                 (split && params.action_prompt ? FlagActionPrompt : 0u) |
                 (split && params.area_name ? FlagAreaName : 0u) |
                 (split && params.area_name && params.area_key ? FlagAreaKey : 0u) |
                 (params.footprint && !params.check ? FlagFootprint : 0u) |
                 (params.menu_box ? FlagMenuBox : 0u) | (marker ? FlagMarker : 0u) |
                 (marker && params.marker_over ? FlagMarkerOver : 0u) |
                 (params.empty_snap ? FlagEmpty : 0u) |
                 (params.slots.on && !params.check ? FlagSheet : 0u) |
                 (params.conditions && params.strip && push.cut_count == 4 ? FlagConditions
                                                                           : 0u) |
                 ((params.status_cuts & 0xFu) << StatusShift) |
                 (dynamic >= 0 ? 1u << (DynamicShift + static_cast<u32>(dynamic)) : 0u);
    const VR::Ui::LockMarker::Factors marker_factors =
        VR::Ui::LockMarker::Clamp(params.marker_scale, params.marker_alpha);
    push.raw_order = RawOrder(components) |
                     (marker ? VR::Ui::LockMarker::Pack(marker_factors) : 0u);
    push.gamma = params.gamma > 0.0f ? params.gamma : 1.0f;
    push.split_px = std::min<u32>(params.split_px[0], 0x7FF) |
                    std::min<u32>(params.split_px[1], 0x7F) << 11 |
                    std::min<u32>(params.split_px[2], 0x7F) << 18 |
                    std::min<u32>(params.split_px[3], 0x7F) << 25;

    const vk::DescriptorImageInfo k_info{.imageView = *targets[Black].sampled,
                                         .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo w_info{.imageView = *targets[Grey].sampled,
                                         .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    // The marker's pair (K and W themselves when there is none: never read then).
    const vk::DescriptorImageInfo mk_info{
        .imageView = marker ? *targets[MarkBlack].sampled : *targets[Black].sampled,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo mw_info{
        .imageView = marker ? *targets[MarkGrey].sampled : *targets[Grey].sampled,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorBufferInfo stats_info{
        .buffer = stats_buffer, .offset = 0, .range = StatsWords * sizeof(u32)};
    const auto sampled = [](u32 binding, const vk::DescriptorImageInfo& info) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = vk::DescriptorType::eSampledImage,
                                      .pImageInfo = &info};
    };
    const auto storage = [](u32 binding, const vk::DescriptorImageInfo& info) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = vk::DescriptorType::eStorageImage,
                                      .pImageInfo = &info};
    };
    const vk::WriteDescriptorSet stats_write{.dstBinding = 3,
                                             .descriptorCount = 1,
                                             .descriptorType = vk::DescriptorType::eStorageBuffer,
                                             .pBufferInfo = &stats_info};
    const vk::DescriptorBufferInfo split_info{
        .buffer = split_buffer, .offset = 0, .range = SplitBytes};
    const vk::WriteDescriptorSet split_write{.dstBinding = 7,
                                             .descriptorCount = 1,
                                             .descriptorType = vk::DescriptorType::eStorageBuffer,
                                             .pBufferInfo = &split_info};
    if (!split_cleared) {
        // The mask starts empty; the split pass clears what it read from then on.
        cmdbuf.fillBuffer(split_buffer, 0, SplitBytes, 0);
        MemoryBarrier(
            cmdbuf, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
        split_cleared = true;
    }
    const auto dispatch = [&](Mode mode, std::span<const vk::WriteDescriptorSet> writes, u32 x,
                              u32 y) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[mode]);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        ScratchPush::Invalidate();
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(x, y, 1);
    };
    const auto stats_written = [&] {
        MemoryBarrier(
            cmdbuf, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderStorageWrite,
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite |
                vk::AccessFlagBits2::eTransferRead);
    };
    const u32 groups_x = DivCeil(params.band.extent.width, 16);
    const u32 groups_y = DivCeil(params.band.extent.height, 16);

    // FAST_GPU_PASS_PROFILE: the resolve's steps apart (the presenter's "VR UI layer resolve"
    // label holds the barriers before them).
    const auto mark = [this](u64 step, const char* name) {
        GpuPassProfiler::MarkAt(scheduler, 0xF11F'0000'0040ull + step,
                                GpuPassProfiler::Group::VrHost, name);
    };
    mark(1, "VR UI resolve: veil estimate and marker box");
    // 1. The veil estimate (and the frame's counters cleared).
    const char fast = ResolveFast();
    {
        const std::array writes{sampled(0, k_info), sampled(1, w_info), stats_write};
        dispatch(fast == '0' ? ModeVeilOriginal : ModeVeil, writes, 1, 1);
        stats_written();
        if (fast == 'v') {
            dispatch(ModeVeilCheck, writes, 1, 1);
            stats_written();
        }
    }
    // 1b. The lock-on marker's box (mode 1 scales it about the box's centre).
    if (marker) {
        const std::array writes{sampled(0, k_info), sampled(1, w_info), stats_write,
                                sampled(8, mk_info), sampled(9, mw_info)};
        dispatch(ModeMarkerBox, writes, groups_x, groups_y);
        stats_written();
    }
    if (params.check) {
        // The check: X against K + T * S.
        const vk::DescriptorImageInfo x_info{.imageView = params.x_final.sampled,
                                             .imageLayout = params.x_final.layout};
        const vk::DescriptorImageInfo s_info{.imageView = scene_ref.sampled,
                                             .imageLayout = scene_ref.layout};
        const std::array writes{sampled(0, k_info), sampled(1, w_info), stats_write,
                                sampled(5, x_info), sampled(6, s_info)};
        dispatch(ModeCheck, writes, groups_x, groups_y);
        stats_written();
    } else {
        // 2. The atlas.
        mark(2, "VR UI resolve: atlas");
        ImageBarrier(cmdbuf, atlas->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eNone,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageWrite);
        const vk::DescriptorImageInfo atlas_info{.imageView = atlas->storage,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const std::array writes{sampled(0, k_info),  sampled(1, w_info),  storage(2, atlas_info),
                                stats_write,         split_write,         sampled(8, mk_info),
                                sampled(9, mw_info)};
        dispatch(ModeAtlas, writes, groups_x, groups_y);
        if (split) {
            mark(3, "VR UI resolve: split");
            // The dynamic region: its rows' HUD extents from the mask, then the split of the
            // atlas (both of its halves were written above).
            const auto compute_to_compute = [&] {
                const vk::MemoryBarrier2 memory{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                     vk::AccessFlagBits2::eShaderStorageWrite,
                };
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &memory,
                });
            };
            compute_to_compute();
            dispatch(fast == 'v' ? ModeSplitRowsCheck : ModeSplitRows, writes, 1, 1);
            compute_to_compute();
            const Region& r = params.cut[dynamic];
            dispatch(ModeSplit, writes, DivCeil(static_cast<u32>(r.x1 - r.x0), 16),
                     DivCeil(static_cast<u32>(r.y1 - r.y0), 16));
        }
        if (params.full_menu && params.status_cuts != 0) {
            // A full menu's frame drawn without the menu: the HUD clusters off the canvas.
            s32 rows = 0;
            for (u32 i = 0; i < push.cut_count; ++i) {
                if ((params.status_cuts >> i) & 1u) {
                    rows = std::max(rows, params.cut[i].y1);
                }
            }
            const vk::MemoryBarrier2 memory{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                 vk::AccessFlagBits2::eShaderStorageWrite,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &memory,
            });
            dispatch(ModeMenuGone, writes, groups_x,
                     DivCeil(static_cast<u32>(std::max(rows, 0)), 16));
        }
        if (menu_hud) {
            // The HUD the game drew under the menu: from K and W as they were before the menu's
            // first draw (or the whole frame's, when nothing but the HUD was drawn), into the
            // bottom half's status cuts (mode 1 left them empty).
            s32 rows = 0;
            for (u32 i = 0; i < push.cut_count; ++i) {
                if ((params.status_cuts >> i) & 1u) {
                    rows = std::max(rows, params.cut[i].y1);
                }
            }
            const vk::MemoryBarrier2 memory{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                 vk::AccessFlagBits2::eShaderStorageWrite,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &memory,
            });
            const bool kept = hud_snap == HudSnap::Kept;
            const vk::DescriptorImageInfo hk_info{
                .imageView = kept ? *hud_keep[Black].sampled : *targets[Black].sampled,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
            const vk::DescriptorImageInfo hw_info{
                .imageView = kept ? *hud_keep[Grey].sampled : *targets[Grey].sampled,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
            const std::array hud_writes{sampled(0, hk_info), sampled(1, hw_info),
                                        storage(2, atlas_info), stats_write, split_write};
            // ModeMenuHud alone uses box as source origin + valid size. Guard by the
            // copied rectangle, not capacity: a smaller later copy leaves stale texels.
            const std::array saved_box{push.box[0], push.box[1], push.box[2], push.box[3]};
            const vk::Rect2D source = kept ? hud_keep_rect : vk::Rect2D{{0, 0}, extent};
            push.box[0] = source.offset.x;
            push.box[1] = source.offset.y;
            push.box[2] = static_cast<s32>(source.extent.width);
            push.box[3] = static_cast<s32>(source.extent.height);
            dispatch(ModeMenuHud, hud_writes, groups_x,
                     DivCeil(static_cast<u32>(std::max(rows, 0)), 16));
            std::copy(saved_box.begin(), saved_box.end(), push.box);
        }
        const auto& sl = params.slots;
        mark(4, "VR UI resolve: menu passes and slot sheet");
        if (sl.on && sl.Rows() > 0) {
            // The slot sheet (mode 9): the margins' blocks below the halves, after the passes
            // above wrote the halves (the atlas's mips are made from the whole image later).
            const vk::MemoryBarrier2 memory{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                 vk::AccessFlagBits2::eShaderStorageWrite,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &memory,
            });
            const auto put = [](s32 (&dst)[4], const std::array<s32, 4>& v) {
                std::copy(v.begin(), v.end(), dst);
            };
            // Its words go in the cut, box and scene size fields, put back after it: the scene
            // veil below reads the scene size (left as the banner rows, the menu's dim darkened
            // only a corner of each eye and none of the rest, 2026-10-27).
            const ResolvePush saved = push;
            put(push.cut[0], {sl.top_src, sl.top_rows, sl.bottom_src, sl.bottom_rows});
            put(push.cut[1], {sl.sheet_row, sl.status_home_dy, 0, 0});
            put(push.cut[2], sl.status_left);
            put(push.cut[3], sl.status_right);
            put(push.box, sl.area);
            push.scene_size[0] = sl.banner_y0;
            push.scene_size[1] = sl.banner_y1;
            const std::array slot_writes{sampled(0, k_info), sampled(1, w_info),
                                         storage(2, atlas_info), stats_write};
            dispatch(ModeSlots, slot_writes, groups_x, DivCeil(sl.Rows(), 16));
            push = saved;
        }
        ImageBarrier(
            cmdbuf, atlas->image, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        stats_written();
        mark(5, "VR UI resolve: scene veil");
        // 3. The veil into the scene the eyes are made from.
        if (params.veil && scene_ref.storage && scene_ref.image) {
            ImageBarrier(
                cmdbuf, scene_ref.image, scene_ref.layout, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
            // One UI atlas, the same full-screen dim applied to each already-rendered eye.
            // These views are disjoint; there is no second scene render or UI resolve.
            // VR_UI_RESOLVE_FAST: as many groups as mode 0 wrote (none without a veil, when every
            // group of a direct dispatch returned at once), read as an indirect dispatch.
            if (fast != '0') {
                MemoryBarrier(cmdbuf, vk::PipelineStageFlagBits2::eComputeShader,
                              vk::AccessFlagBits2::eShaderStorageWrite,
                              vk::PipelineStageFlagBits2::eDrawIndirect,
                              vk::AccessFlagBits2::eIndirectCommandRead);
            }
            for (const auto view : {scene_ref.storage, scene_ref.right_storage}) {
                if (!view) continue;
                const vk::DescriptorImageInfo scene_info{.imageView = view,
                                                         .imageLayout = vk::ImageLayout::eGeneral};
                const std::array writes2{stats_write, storage(4, scene_info)};
                if (fast == '0') {
                    dispatch(ModeSceneVeil, writes2, DivCeil(scene_ref.extent.width, 16),
                             DivCeil(scene_ref.extent.height, 16));
                    continue;
                }
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[ModeSceneVeil]);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0,
                                            writes2);
                ScratchPush::Invalidate();
                cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(push), &push);
                cmdbuf.dispatchIndirect(stats_buffer, WVeilArgs * sizeof(u32));
            }
            ImageBarrier(cmdbuf, scene_ref.image, vk::ImageLayout::eGeneral, scene_ref.layout,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderStorageWrite,
                         vk::PipelineStageFlagBits2::eComputeShader |
                             vk::PipelineStageFlagBits2::eFragmentShader,
                         vk::AccessFlagBits2::eShaderSampledRead);
        }
    }
    // The words into this frame's readback slot (read a few frames later, never waited for).
    mark(6, "VR UI resolve: stats copy");
    CollectStats();
    const u32 slot = readback_slot;
    readback_slot = (readback_slot + 1) % ReadbackSlots;
    const vk::BufferCopy copy{
        .srcOffset = 0,
        .dstOffset = slot * StatsWords * sizeof(u32),
        .size = StatsWords * sizeof(u32),
    };
    cmdbuf.copyBuffer(stats_buffer, readback_buffer, copy);
    MemoryBarrier(cmdbuf, vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eHost,
                  vk::AccessFlagBits2::eHostRead);
    readback_pending[slot] = true;
    readback_tick[slot] = scheduler.CurrentTick();
    readback_seq[slot] = params.seq;
    readback_cls[slot] = params.cls;
    readback_split[slot] = split;
    readback_hud[slot] =
        !params.check && params.menu_hud && params.status_cuts != 0
            ? static_cast<u8>(1 + static_cast<u8>(hud_snap))
            : 0;
    readback_marker[slot] = marker;
    readback_marker_scale[slot] = marker_factors.scale;
    readback_marker_alpha[slot] = marker_factors.alpha;
    if ((timing_written & 0x3) == 0x3) {
        Timestamp(cmdbuf, 2);
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return true;
}

void VrUiLayer::CollectStats() {
    // The finished slots, oldest first.
    std::array<u32, ReadbackSlots> ready{};
    u32 ready_count = 0;
    for (u32 slot = 0; slot < ReadbackSlots; ++slot) {
        if (readback_pending[slot] && scheduler.IsFree(readback_tick[slot])) {
            ready[ready_count++] = slot;
        }
    }
    std::sort(ready.begin(), ready.begin() + ready_count,
              [&](u32 a, u32 b) { return readback_tick[a] < readback_tick[b]; });
    static const bool trace = std::getenv("VR_UI_LOG") != nullptr;
    for (u32 n = 0; n < ready_count; ++n) {
        const u32 slot = ready[n];
        readback_pending[slot] = false;
        vmaInvalidateAllocation(allocator, readback_allocation, slot * StatsWords * sizeof(u32),
                                StatsWords * sizeof(u32));
        const u32* w = readback_words + slot * StatsWords;
        auto& s = stats_sum;
        ++s.frames;
        if (ResolveFast() == 'v') {
            static u64 compared = 0;
            static u64 differed = 0;
            static u64 split_rows_differed = 0;
            compared += w[WVerifyN] != 0 ? 1 : 0;
            differed += w[WVerifyN] != 0 && w[WVerifyBad] != 0 ? 1 : 0;
            const u32 split_now = w[WVerifyN] != 0 ? w[WVerifySplit] : 0;
            split_rows_differed += split_now;
            if (w[WVerifyN] != 0 &&
                (compared % 600 == 0 || ((w[WVerifyBad] != 0 || split_now != 0) &&
                                         differed + split_rows_differed <= 5 + split_now))) {
                LOG_INFO(Render_Vulkan,
                         "VR_UI_RESOLVE_FAST=v: {} frames compared, {} with a veil word that "
                         "differs from the original pass, {} split rows that differ (this frame "
                         "veil {} args {} {} {}, split rows found {})",
                         compared, differed, split_rows_differed, w[WVeilOn], w[WVeilArgs],
                         w[WVeilArgs + 1], w[WVeilArgs + 2], w[WSplitRows]);
            }
        }
        const float veil = std::bit_cast<float>(w[WVeil]);
        if (w[WVeilOn] != 0) {
            ++s.veil_frames;
            s.veil_min = std::min(s.veil_min, veil);
        }
        s.veil_last = w[WVeilOn] != 0 ? veil : 1.0f;
        if (w[WVeilOn] != 0) {
            for (u32 c = 0; c < 3; ++c) {
                s.veil_t[c] = std::bit_cast<float>(w[WVeilT + c]);
                s.veil_k[c] = std::bit_cast<float>(w[WVeilK + c]);
            }
        }
        s.strip_k_spread = std::bit_cast<float>(w[WStripKd]);
        // The newest frame's content box (the slots are read in order of completion; the newest
        // tick wins).
        if (readback_tick[slot] >= box_tick) {
            box_tick = readback_tick[slot];
            latest_box.valid =
                w[WBox + 2] >= w[WBox] && w[WBox + 3] >= w[WBox + 1] && w[WBox] != 0xFFFFFFFFu;
            latest_box.x0 = static_cast<s32>(w[WBox]);
            latest_box.y0 = static_cast<s32>(w[WBox + 1]);
            latest_box.x1 = static_cast<s32>(w[WBox + 2]) + 1;
            latest_box.y1 = static_cast<s32>(w[WBox + 3]) + 1;
            latest_box.regions = w[WRest] != 0 ? 1u << 4 : 0u;
            for (u32 i = 0; i < 4; ++i) {
                latest_box.regions |= w[WCut0 + i] != 0 ? 1u << i : 0u;
                const u32* c = w + WCutBox + 4 * i;
                latest_box.cut_box[i] =
                    c[2] >= c[0] && c[3] >= c[1] && c[0] != 0xFFFFFFFFu
                        ? std::array<s32, 4>{static_cast<s32>(c[0]), static_cast<s32>(c[1]),
                                             static_cast<s32>(c[2]) + 1,
                                             static_cast<s32>(c[3]) + 1}
                        : std::array<s32, 4>{};
            }
            latest_box.border = w[WBorder];
            latest_box.slot_area = w[WSlotArea];
            latest_box.slot_banner = w[WSlotBanner];
            latest_box.hud_right = static_cast<s32>(w[WSplitRight]);
            latest_box.content = w[WContent];
            latest_box.opaque = w[WOpaque];
            {
                const u32* o = w + WOpaqueBox;
                latest_box.opaque_box =
                    o[2] >= o[0] && o[3] >= o[1] && o[0] != 0xFFFFFFFFu
                        ? std::array<s32, 4>{static_cast<s32>(o[0]), static_cast<s32>(o[1]),
                                             static_cast<s32>(o[2]) + 1, static_cast<s32>(o[3]) + 1}
                        : std::array<s32, 4>{};
            }
            latest_box.seq = readback_seq[slot];
            latest_box.cls = readback_cls[slot];
        }
        if (trace) {
            // The frames around a change of the menu class: does a frame tagged with one class
            // already (or still) draw the other one's UI?
            const ClassTrace entry{readback_seq[slot], readback_cls[slot], w[WContent], w[WRest]};
            const u32 size = static_cast<u32>(class_trace.size());
            if (class_trace_count != 0 &&
                class_trace[(class_trace_count - 1) % size].cls != entry.cls) {
                std::string before;
                for (u32 i = class_trace_count > size ? class_trace_count - size : 0;
                     i < class_trace_count; ++i) {
                    const auto& e = class_trace[i % size];
                    before +=
                        fmt::format(" [{} c{} {} px, rest {}]", e.seq, e.cls, e.content, e.rest);
                }
                LOG_INFO(Render_Vulkan, "VR ui trace: class {} from frame {}; before:{}", entry.cls,
                         entry.seq, before);
                class_trace_after = 6;
            }
            if (class_trace_after != 0) {
                --class_trace_after;
                LOG_INFO(Render_Vulkan, "VR ui trace: frame {} class {}: {} px, rest {}", entry.seq,
                         entry.cls, entry.content, entry.rest);
            }
            class_trace[class_trace_count % size] = entry;
            ++class_trace_count;
        }
        s.strip_t = std::bit_cast<float>(w[WStripT]);
        s.strip_spread = std::bit_cast<float>(w[WStripD]);
        s.strip_k = std::bit_cast<float>(w[WStripK]);
        s.content += w[WContent];
        s.rest += w[WRest];
        s.border += w[WBorder];
        s.opaque += w[WOpaque];
        for (u32 i = 0; i < 4; ++i) {
            s.cut[i] += w[WCut0 + i];
        }
        s.check_n += w[WCheckN];
        s.check_bad += w[WCheckBad];
        s.check_bad8 += w[WCheckBad8];
        s.check_sum += w[WCheckSum];
        s.check_max = std::max(s.check_max, w[WCheckMax]);
        s.last_content = w[WContent];
        s.saturated += w[WSaturated];
        s.menu_gone += w[WMenuGone] != 0 ? 1u : 0u;
        switch (readback_hud[slot]) {
        case 1 + static_cast<u8>(HudSnap::Whole):
            ++s.menu_hud_whole;
            break;
        case 1 + static_cast<u8>(HudSnap::Kept):
            ++s.menu_hud_kept;
            break;
        case 1 + static_cast<u8>(HudSnap::Empty):
            ++s.menu_hud_empty;
            break;
        default:
            break;
        }
        if (readback_marker[slot]) {
            ++s.marker_frames;
            const u32* b = w + WMarkBox;
            if (b[2] >= b[0] && b[3] >= b[1] && b[0] != 0xFFFFFFFFu) {
                s.marker_box = {static_cast<s32>(b[0]), static_cast<s32>(b[1]),
                                static_cast<s32>(b[2]), static_cast<s32>(b[3])};
            } else {
                ++s.marker_empty;
            }
            s.marker_scale = readback_marker_scale[slot];
            s.marker_alpha = readback_marker_alpha[slot];
        }
        s.menu_hud_px += w[WMenuHud];
        s.menu_hud_out += w[WMenuHudOut];
        s.menu_hud_veiled += w[WMenuHudVeil] != 0 ? 1u : 0u;
        if (readback_split[slot]) {
            ++s.split_frames;
            s.split_rows += w[WSplitRows];
            s.split_out += w[WSplitOut];
        }
    }
}

VrUiLayer::Stats VrUiLayer::TakeStats() {
    CollectStats();
    return std::exchange(stats_sum, Stats{});
}

void VrUiLayer::EnableTiming(const Instance& inst) {
    if (query_pool) {
        return;
    }
    query_pool = Check<"create vr ui query pool">(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = TimingSlots * 4,
    }));
    timestamp_period_ns = inst.GetPhysicalDevice().getProperties().limits.timestampPeriod;
}

void VrUiLayer::MarkComposite(vk::CommandBuffer cmdbuf) {
    if (query_pool) {
        Timestamp(cmdbuf, 0);
    }
}

void VrUiLayer::MarkFlip(vk::CommandBuffer cmdbuf) {
    if (query_pool && (timing_written & 0x1) != 0 && (timing_written & 0x2) == 0) {
        Timestamp(cmdbuf, 1);
        Timestamp(cmdbuf, 2);
    }
}

void VrUiLayer::Timestamp(vk::CommandBuffer cmdbuf, u32 query) {
    if (!query_pool) {
        return;
    }
    if (query == 0) {
        if (timing_written != 0) {
            timing_slot = (timing_slot + 1) % TimingSlots; // a frame without a resolve
        }
        timing_written = 0;
        timing_pending[timing_slot] = false;
        cmdbuf.resetQueryPool(*query_pool, timing_slot * 4, 4);
        CollectTimes();
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *query_pool,
                           timing_slot * 4 + query);
    timing_written |= 1u << query;
}

void VrUiLayer::CollectTimes() {
    for (u32 slot = 0; slot < TimingSlots; ++slot) {
        if (!timing_pending[slot] || slot == timing_slot) {
            continue;
        }
        std::array<u64, 3> ticks{};
        const vk::Result result =
            device.getQueryPoolResults(*query_pool, slot * 4, 3, sizeof(ticks), ticks.data(),
                                       sizeof(u64), vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess) {
            continue; // not finished yet; never waited for
        }
        timing_pending[slot] = false;
        const float to_ms = timestamp_period_ns * 1e-6f;
        const float ui = static_cast<float>(ticks[1] - ticks[0]) * to_ms;
        const float resolve = static_cast<float>(ticks[2] - ticks[1]) * to_ms;
        ++timing_sum.frames;
        timing_sum.ui_ms += ui;
        timing_sum.resolve_ms += resolve;
        timing_sum.ui_max_ms = std::max(timing_sum.ui_max_ms, ui);
        timing_sum.resolve_max_ms = std::max(timing_sum.resolve_max_ms, resolve);
    }
}

VrUiLayer::GpuTimes VrUiLayer::TakeGpuTimes() {
    if (query_pool) {
        CollectTimes();
    }
    GpuTimes result = std::exchange(timing_sum, GpuTimes{});
    if (result.frames != 0) {
        result.ui_ms /= static_cast<float>(result.frames);
        result.resolve_ms /= static_cast<float>(result.frames);
    }
    return result;
}

} // namespace Vulkan
