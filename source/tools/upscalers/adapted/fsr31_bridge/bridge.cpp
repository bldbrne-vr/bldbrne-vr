// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Production adapter. Same single-eye ownership/timeline contract as the FSR4 bridge.
// FSR3.1.5 upscaling only; no frame generation, submits or internal waits.
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/renderer_vulkan/vr_fsr4_bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>
namespace {
using Resource = FfxVkFsr3_3_1_5Resource;
constexpr auto Usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
struct Owned {
    VkImage image{};
    VkDeviceMemory memory{};
    Resource resource{};
};
struct Imported {
    VkImage image{};
    VkFormat format{};
    uint32_t width{}, height{};
    Resource resource{};
};
struct Context {
    VkDevice device{};
    VkPhysicalDevice physical{};
    VrFsr4Desc desc{};
    FfxVkFsr3_3_1_5UpscalerContext *sdk{};
    std::array<Owned, 3> shared{};
    std::vector<Imported> imported;
    uint64_t submitted{}, completed{};
    bool initialized{}, failed{};
    uint32_t render_width{}, render_height{};
    const char *problem = "FSR 3.1.5 diagnostic provider";
    ~Context() {
        if (sdk)
            ffxVkFsr3_3_1_5UpscalerContextDestroy(sdk);
        for (auto &s : shared) {
            if (s.image)
                vkDestroyImage(device, s.image, nullptr);
            if (s.memory)
                vkFreeMemory(device, s.memory, nullptr);
        }
    }
    Resource Import(VkImage image, VkFormat format, uint32_t w, uint32_t h, uint32_t state) {
        for (auto &i : imported)
            if (i.image == image && i.format == format && i.width == w && i.height == h)
                return i.resource;
        FfxVkFsr3_3_1_5ImportedImageDescription d{
            image, format, w, h, 1, 1, VK_IMAGE_LAYOUT_GENERAL, state, Usage};
        auto r = ffxVkFsr3_3_1_5BridgeImportImage(ffxVkFsr3_3_1_5UpscalerContextGetBridge(sdk), &d);
        if (r.resource)
            imported.push_back({image, format, w, h, r});
        return r;
    }
    bool Allocate(Owned &s, const FfxVkFsr3_3_1_5SharedResourceDescription &d) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = d.format;
        ci.extent = {d.width, d.height, 1};
        ci.mipLevels = d.mipCount;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = Usage;
        if (vkCreateImage(device, &ci, nullptr, &s.image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, s.image, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t type = props.memoryTypeCount;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        if (type == props.memoryTypeCount)
            return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &s.memory) != VK_SUCCESS ||
            vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS)
            return false;
        s.resource = Import(s.image, d.format, d.width, d.height,
                            FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS);
        return s.resource.resource != nullptr;
    }
};
void *Create(VkDevice device, VkPhysicalDevice physical, const wchar_t *, const VrFsr4Desc *desc) {
    if (!device || !physical || !desc || desc->profile > 3 || desc->depth_inverted ||
        !desc->width || !desc->height || desc->width > 16384 || desc->height > 16384)
        return nullptr;
    auto c = std::make_unique<Context>();
    c->device = device;
    c->physical = physical;
    c->desc = *desc;
    // The preset is a preference, not the actual render size: the game's boot
    // resolution survives live preset edits. Allocate on the first validated frame.
    return c.release();
}
int32_t Evaluate(void *opaque, const VrFsr4Frame *f) {
    auto &c = *static_cast<Context *>(opaque);
    if (!f || c.failed || !f->command || f->ticket <= c.completed || f->ticket < c.submitted ||
        !f->color.image || !f->depth.image || !f->motion.image || !f->output.image)
        return 0;
    const auto w = f->color.width, h = f->color.height;
    if (!w || !h || w > c.desc.width || h > c.desc.height ||
        f->depth.width != w || f->depth.height != h ||
        f->motion.width != w || f->motion.height != h ||
        f->output.width != c.desc.width || f->output.height != c.desc.height ||
        (c.sdk && (w > c.render_width || h > c.render_height))) {
        c.problem = "FSR 3.1.5 frame dimensions exceed context or disagree";
        return 0; // No imports or GPU commands on invalid input.
    }
    if (!c.sdk) {
        c.render_width = w; c.render_height = h;
        FfxVkFsr3_3_1_5UpscalerCreateInfo info{
            c.physical, c.device, w, h, c.desc.width, c.desc.height, VK_FALSE, VK_TRUE};
        FfxVkFsr3_3_1_5SharedResourceDescriptions shared{};
        if (ffxVkFsr3_3_1_5UpscalerContextCreate(&info, &c.sdk) != 0 || !c.sdk ||
            ffxVkFsr3_3_1_5UpscalerContextGetSharedResourceDescriptions(c.sdk, &shared) != 0) {
            c.failed = true; c.problem = "FSR 3.1.5 context allocation failed"; return 0;
        }
        const std::array descriptions{shared.dilatedDepth, shared.dilatedMotionVectors,
                                      shared.reconstructedPrevNearestDepth};
        for (unsigned i = 0; i < descriptions.size(); ++i) {
            if (!c.Allocate(c.shared[i], descriptions[i])) {
                c.failed = true; c.problem = "FSR 3.1.5 shared image allocation failed"; return 0;
            }
        }
    }
    if (!c.initialized) {
        std::array<VkImageMemoryBarrier, 3> barriers{};
        for (unsigned i = 0; i < 3; ++i) {
            auto &b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = c.shared[i].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        }
        vkCmdPipelineBarrier(f->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                             barriers.data());
        c.initialized = true;
    }
    constexpr auto read = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ;
    FfxVkFsr3_3_1_5UpscalerDispatchInfo d{};
    d.commandBuffer = f->command;
    d.color = c.Import(f->color.image, VK_FORMAT_R16G16B16A16_SFLOAT, f->color.width,
                       f->color.height, read);
    d.depth = c.Import(f->depth.image, VK_FORMAT_R32_SFLOAT, f->depth.width, f->depth.height, read);
    d.motionVectors =
        c.Import(f->motion.image, VK_FORMAT_R16G16_SFLOAT, f->motion.width, f->motion.height, read);
    d.output = c.Import(f->output.image, VK_FORMAT_R16G16B16A16_SFLOAT, f->output.width,
                        f->output.height, FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS);
    d.dilatedDepth = c.shared[0].resource;
    d.dilatedMotionVectors = c.shared[1].resource;
    d.reconstructedPrevNearestDepth = c.shared[2].resource;
    d.jitterOffsetX = f->jitter_x;
    d.jitterOffsetY = f->jitter_y;
    d.motionVectorScaleX = d.motionVectorScaleY = 1;
    d.renderWidth = f->color.width;
    d.renderHeight = f->color.height;
    d.upscaleWidth = f->output.width;
    d.upscaleHeight = f->output.height;
    d.frameTimeMilliseconds = std::max(f->frame_ms, 0.01f);
    d.preExposure = 1;
    d.reset = f->reset;
    d.cameraNear = f->near_plane;
    d.cameraFar = f->far_plane;
    d.cameraVerticalFovRadians = f->vertical_fov;
    d.viewSpaceToMeters = 1;
    d.frameId = f->ticket;
    // Sharpening disabled, matching the temporal benchmark's FSR4 settings.
    c.submitted = f->ticket;
    if (ffxVkFsr3_3_1_5UpscalerContextRecordDispatch(c.sdk, &d) != 0) {
        c.problem = "FSR 3.1.5 dispatch failed";
        return 0;
    }
    return 1;
}
void Completed(void *p, uint64_t tick) {
    auto &c = *static_cast<Context *>(p);
    c.completed = std::max(c.completed, tick);
    if (c.sdk) ffxVkFsr3_3_1_5UpscalerContextRetireFrame(c.sdk, c.completed);
}
int32_t Release(void *p) {
    auto *c = static_cast<Context *>(p);
    if (c->submitted > c->completed)
        return 0;
    delete c;
    return 1;
}
const char *Problem(void *p) { return static_cast<Context *>(p)->problem; }
const VrFsr4Api api{VR_FSR4_ABI_V1, Create, Evaluate, Completed, Release, Problem};
} // namespace
extern "C" __declspec(dllexport) const VrFsr4Api *VrFsr4GetStereoApi() { return &api; }
