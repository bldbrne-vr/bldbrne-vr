#include "../../../scratchpad/dual-eye-perf/push_state_memo.h"
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_smaa.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <vk_mem_alloc.h>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_smaa_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_smaa_trim.h"

// SMAA's precomputed lookup textures (MIT, the reference's own headers with their notice).
#include "video_core/renderer_vulkan/smaa/AreaTex.h"
#include "video_core/renderer_vulkan/smaa/SearchTex.h"

namespace Vulkan {

namespace {

/// Mirrors SmaaPush in vr_smaa.comp.
struct SmaaPush {
    float rt_metrics[4];
    s32 size[2];
    u32 flags;
    u32 pad;
};
static_assert(sizeof(SmaaPush) == 32);

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr u32 Group = 8;                        // vr_smaa.comp's local size
constexpr u64 TimerKey = 0x534d414100000001ull; // "SMAA": the `VR gpu passes:` line (cs 534d...)

u32 DivCeil(u32 a, u32 b) {
    return (a + b - 1) / b;
}

// A barrier for one of our images kept in GENERAL: the compute writes before -> compute reads and
// writes after.
vk::ImageMemoryBarrier2 ComputeBarrier(vk::Image image, bool first_use) {
    return {
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = first_use ? vk::AccessFlagBits2::eNone
                                   : vk::AccessFlagBits2::eShaderStorageWrite |
                                         vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = first_use ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

// FAST_SMAA_RECORDER_VERIFY=N: every Nth frame recorded with FAST bit 41's per-eye intermediates
// is checked on the GPU against the eye-major order (Record). 0 / unset: never.
u64 SmaaVerifyEvery() {
    static const u64 every = [] {
        const char* value = std::getenv("FAST_SMAA_RECORDER_VERIFY");
        return value ? static_cast<u64>(std::strtoull(value, nullptr, 0)) : u64{0};
    }();
    return every;
}
// Written on the scheduler's priority-operation thread, read by the recording thread.
std::atomic<u64> g_smaa_verify_compared{0};
std::atomic<u64> g_smaa_verify_mismatched{0};

// FAST_GPU_SMAA_TRIM (read once at start-up; unset or 0: off, SMAA exactly as before): a mask of
// two trims of SMAA's GPU passes (research_smaa.md items 3 and 4; its items 1 and 2, the recorder
// and the per-eye intermediates, are FAST bit 41). Both are off by default:
//   1 (SmaaTrim::Keep, vr_smaa.comp SMAA_KEEP): the blend pass stores nothing where no weight
//     applies, instead of B's texel: A keeps its own bytes there. That is most pixels (5-8% of a
//     frame's pixels have an edge, measured on eye captures), so B's fetch and A's store (8
//     bytes) go at ~85-90% of the pixels: about 7 of the ~24 bytes per pixel SMAA's three passes
//     move. Exact only if the game's copy draw (fs 0xf7221902253) left B byte for byte equal to
//     A. That is not known: if the copy writes something else into B's alpha (FXAA's luma
//     convention) or touches the colour, those pixels of A differ from today's
//     (FAST_GPU_SMAA_TRIM_VERIFY says how many, colour or alpha only). Never when the FXAA draw's
//     pass began A with a clear (Frame::dst_cleared).
//   2 (SmaaTrim::Compact, SMAA_WEIGHTS + SMAA_COMPACT): the weights pass in 16x16 groups with
//     the pixels that have an edge packed into the first warps (the parked FAST bit 42 patch). The
//     same texels by construction (the shader's comment; glslang emits the 8x8 pass's SPIR-V for
//     every SMAA function it calls). Fewer search warps are issued, but a 16x16 group keeps its
//     eight warps' slots until its last search ends, and 40-55% of the 8x8 tiles of a captured
//     frame hold an edge: with searches bound by fetch latency, a quarter as many resident groups
//     may make it slower. Time it (VR_GPU_PASSES, cs 534d414100000001) before turning it on.
// Applied per frame (the same for both eyes); nothing is carried between frames: every weights
// texel is rewritten either way, and the bytes Keep leaves are this frame's A.
const u32 g_smaa_trim = SmaaTrim::ParseMask(std::getenv("FAST_GPU_SMAA_TRIM"));
// FAST_GPU_SMAA_TRIM_VERIFY=N: every Nth trimmed frame (1: every one), A is copied to a readback
// buffer, the full passes run again in the same order (A keeps today's pixels), A is copied again
// and the two are compared on the priority-operation thread: colour and alpha-only texel counts,
// the first difference in an error line (the first few frames), totals in the "FAST smaa_trim"
// line. 0 / unset: never.
const u64 g_smaa_trim_verify =
    g_smaa_trim != 0 ? SmaaTrim::ParseEvery(std::getenv("FAST_GPU_SMAA_TRIM_VERIFY")) : 0;
// FAST_GPU_SMAA_TRIM_VERIFY results (priority-operation thread -> the recording thread's line).
std::atomic<u64> g_trim_compared{0};
std::atomic<u64> g_trim_differing{0}; // checks with any difference
std::atomic<u64> g_trim_color_texels{0};
std::atomic<u64> g_trim_alpha_texels{0};
std::atomic<u32> g_trim_error_lines{0};
constexpr u32 TrimErrorLines = 8; // error lines per session; then only the totals

} // Anonymous namespace

VrSmaa::VrSmaa(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()} {
    ready = Init();
}

VrSmaa::~VrSmaa() {
    for (const Views& v : views) {
        if (v.unorm) {
            device.destroyImageView(v.unorm);
        }
        if (v.srgb) {
            device.destroyImageView(v.srgb);
        }
    }
}

void VrSmaa::Fail(const char* what) {
    LOG_ERROR(Render_Vulkan,
              "VR smaa: unavailable ({}); aa_mode=smaa draws the game's FXAA instead", what);
    ready = false;
    failed = true;
}

int VrSmaa::ByteOrder(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eA8B8G8R8UnormPack32:
    case vk::Format::eA8B8G8R8SrgbPack32:
        return 0;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return 1;
    default:
        return -1;
    }
}

bool VrSmaa::Init() {
    // Edges: RG8 storage where the device has it (every desktop GPU), else RGBA8.
    if (!instance.IsFormatSupported(vk::Format::eR8G8Unorm,
                                    vk::FormatFeatureFlagBits2::eStorageImage |
                                        vk::FormatFeatureFlagBits2::eSampledImageFilterLinear)) {
        edges_format = vk::Format::eR8G8B8A8Unorm;
    }
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

    std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i == 6 ? vk::DescriptorType::eStorageImage
                                     : vk::DescriptorType::eCombinedImageSampler,
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
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(SmaaPush),
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
        const vk::ShaderModule module =
            Compile(HostShaders::VR_SMAA_COMP, vk::ShaderStageFlagBits::eCompute, device,
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
    std::vector<std::string> edge_defines{"SMAA_EDGES"};
    if (edges_format != vk::Format::eR8G8Unorm) {
        edge_defines.emplace_back("SMAA_EDGES_RGBA8");
    }
    if (!make(edge_defines, "vr_smaa.comp (edges)", edges_pipeline) ||
        !make({"SMAA_WEIGHTS"}, "vr_smaa.comp (weights)", weights_pipeline) ||
        !make({"SMAA_BLEND"}, "vr_smaa.comp (blend)", blend_pipeline)) {
        Fail("pipelines");
        return false;
    }
    // FAST_GPU_SMAA_TRIM's variants, same layout (a failure leaves that trim off: its full pass).
    if ((g_smaa_trim & SmaaTrim::Keep) != 0 &&
        !make({"SMAA_BLEND", "SMAA_KEEP"}, "vr_smaa.comp (blend, keep)", blend_keep_pipeline)) {
        blend_keep_pipeline.reset();
        LOG_WARNING(Render_Vulkan, "FAST smaa_trim: no keep pipeline; the full blend runs");
    }
    if ((g_smaa_trim & SmaaTrim::Compact) != 0 &&
        !make({"SMAA_WEIGHTS", "SMAA_COMPACT"}, "vr_smaa.comp (weights, compact)",
              weights_compact_pipeline)) {
        weights_compact_pipeline.reset();
        LOG_WARNING(Render_Vulkan, "FAST smaa_trim: no compact pipeline; the 8x8 weights run");
    }
    if (g_smaa_trim != 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST smaa_trim: FAST_GPU_SMAA_TRIM={} (keep {}, compact {}); verify every {} "
                 "trimmed frames (0: never)",
                 g_smaa_trim, blend_keep_pipeline ? "on" : "off",
                 weights_compact_pipeline ? "on" : "off", g_smaa_trim_verify);
    }

    // The lookup textures (uploaded at the first Record).
    const auto lut = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                         vk::Format format, u32 w, u32 h) {
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto v = device.createImageViewUnique({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = ColorRange,
        });
        if (v.result != vk::Result::eSuccess) {
            return false;
        }
        view = std::move(v.value);
        return true;
    };
    if (!lut(area_image, area_view, vk::Format::eR8G8Unorm, AREATEX_WIDTH, AREATEX_HEIGHT) ||
        !lut(search_image, search_view, vk::Format::eR8Unorm, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT)) {
        Fail("lookup textures");
        return false;
    }
    LOG_INFO(Render_Vulkan, "VR smaa: ready (SMAA 1x, preset high; edges {})",
             vk::to_string(edges_format));
    return true;
}

void VrSmaa::Upload(const RecorderProxy& cmdbuf) {
    const VkDeviceSize area_size = sizeof(areaTexBytes);
    const VkDeviceSize search_size = sizeof(searchTexBytes);
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = area_size + search_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    const VmaAllocationCreateInfo alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo out{};
    const VmaAllocator allocator = instance.GetAllocator();
    if (vmaCreateBuffer(allocator, &info, &alloc, &buffer, &allocation, &out) != VK_SUCCESS ||
        out.pMappedData == nullptr) {
        Fail("staging buffer");
        return;
    }
    std::memcpy(out.pMappedData, areaTexBytes, area_size);
    std::memcpy(static_cast<u8*>(out.pMappedData) + area_size, searchTexBytes, search_size);
    vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);

    std::array<vk::ImageMemoryBarrier2, 2> to_dst{};
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    const std::array<vk::Image, 2> images{area_image, search_image};
    for (size_t i = 0; i < 2; ++i) {
        to_dst[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = images[i],
            .subresourceRange = ColorRange,
        };
        to_read[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = images[i],
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_dst.size()),
        .pImageMemoryBarriers = to_dst.data(),
    });
    const vk::BufferImageCopy area_copy{
        .bufferOffset = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1},
    };
    const vk::BufferImageCopy search_copy{
        .bufferOffset = area_size,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1},
    };
    cmdbuf.copyBufferToImage(vk::Buffer{buffer}, area_image, vk::ImageLayout::eTransferDstOptimal,
                             area_copy);
    cmdbuf.copyBufferToImage(vk::Buffer{buffer}, search_image, vk::ImageLayout::eTransferDstOptimal,
                             search_copy);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_read.size()),
        .pImageMemoryBarriers = to_read.data(),
    });
    scheduler.DeferOperation(
        [allocator, buffer, allocation] { vmaDestroyBuffer(allocator, buffer, allocation); });
    uploaded = true;
}

bool VrSmaa::EnsureTarget(Target& t, vk::Extent2D extent, vk::Format format, const char* name) {
    if (t.image && t.extent == extent && t.format == format) {
        return true;
    }
    if (t.image) {
        // Work in flight may still use the old one.
        scheduler.DeferOperation([image = std::move(t.image), view = std::move(t.view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    t.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    t.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
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
    t.extent = extent;
    t.format = format;
    t.fresh = true;
    return true;
}

bool VrSmaa::EnsureTargets(vk::Extent2D extent) {
    const bool changed = edges.extent != extent;
    if (!EnsureTarget(edges, extent, edges_format, "vr_smaa edges") ||
        !EnsureTarget(weights, extent, vk::Format::eR8G8B8A8Unorm, "vr_smaa weights")) {
        return false;
    }
    if (changed) {
        LOG_INFO(Render_Vulkan, "VR smaa: {}x{} (edges {}, weights R8G8B8A8Unorm)", extent.width,
                 extent.height, vk::to_string(edges_format));
    }
    return true;
}

const VrSmaa::Views* VrSmaa::ViewsOf(vk::Image image, u64 uid, vk::Format format, bool srgb,
                                     u32 layer) {
    for (Views& v : views) {
        if (v.image == image && v.uid == uid && v.layer == layer && v.sampled == srgb) {
            v.last_use = frame;
            return &v;
        }
    }
    // A game image we have not seen (the first frame, a new resolution): views of it. Views of
    // images not used for a while go (their image may be gone; the GPU may still use them).
    std::erase_if(views, [&](const Views& v) {
        if (frame - v.last_use < 600) {
            return false;
        }
        scheduler.DeferOperation([dev = device, a = v.unorm, b = v.srgb] {
            if (a) {
                dev.destroyImageView(a);
            }
            if (b) {
                dev.destroyImageView(b);
            }
        });
        return true;
    });
    (void)format;
    Views v{.image = image, .uid = uid, .layer = layer, .sampled = srgb, .last_use = frame};
    auto range = ColorRange;
    range.baseArrayLayer = layer;
    vk::ImageViewUsageCreateInfo sampled_usage{.usage = vk::ImageUsageFlagBits::eSampled};
    vk::ImageViewUsageCreateInfo storage_usage{.usage = vk::ImageUsageFlagBits::eStorage};
    auto un = device.createImageView({
        .pNext = srgb ? &sampled_usage : &storage_usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = range,
    });
    if (un.result != vk::Result::eSuccess) {
        return nullptr;
    }
    v.unorm = un.value;
    if (srgb) {
        auto sr = device.createImageView({
            .pNext = &sampled_usage,
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Srgb,
            .subresourceRange = range,
        });
        if (sr.result != vk::Result::eSuccess) {
            device.destroyImageView(v.unorm);
            return nullptr;
        }
        v.srgb = sr.value;
    }
    views.push_back(v);
    return &views.back();
}

VrSmaa::Stats VrSmaa::GetStats() const {
    return {
        .per_eye_frames = per_eye_frames,
        .barrier_calls = barrier_calls,
        .verify_recorded = verify_recorded,
        .verify_compared = g_smaa_verify_compared.load(std::memory_order_relaxed),
        .verify_mismatched = g_smaa_verify_mismatched.load(std::memory_order_relaxed),
    };
}

void VrSmaa::ReportTrim(bool keep, bool compact) const {
    // FAST_GPU_SMAA_TRIM's line, every 600 SMAA frames while a trim runs (the recording thread;
    // the verify totals arrive from the priority-operation thread).
    if (frame % 600 != 0) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "FAST smaa_trim: frames={} trimmed={} keep={} compact={} verify_recorded={} "
             "compared={} differing={} colour_texels={} alpha_only_texels={}",
             frame, trim_frames, keep ? 1 : 0, compact ? 1 : 0, trim_verify_recorded,
             g_trim_compared.load(std::memory_order_relaxed),
             g_trim_differing.load(std::memory_order_relaxed),
             g_trim_color_texels.load(std::memory_order_relaxed),
             g_trim_alpha_texels.load(std::memory_order_relaxed));
}

bool VrSmaa::Record(const RecorderProxy& cmdbuf, const Frame& f) {
    ++frame;
    const auto refuse = [&](u32 bit, const char* why) {
        if ((warned & bit) == 0) {
            warned |= bit;
            LOG_WARNING(Render_Vulkan, "VR smaa: {}; such frames draw the game's FXAA", why);
        }
        return false;
    };
    if (!ready) {
        return false;
    }
    const int src_order = ByteOrder(f.src_format);
    const int dst_order = ByteOrder(f.dst_format);
    if (src_order < 0 || dst_order < 0) {
        return refuse(1u << 0, "the FXAA images are not 8-bit RGBA / BGRA");
    }
    if (src_order != dst_order) {
        return refuse(1u << 1, "the FXAA texture and target differ in byte order");
    }
    if (!f.src || !f.dst || f.src == f.dst || f.extent.width == 0 || f.extent.height == 0) {
        return refuse(1u << 2, "no FXAA texture / target");
    }
    if (f.layers < 1 || f.layers > 2)
        return refuse(1u << 5, "invalid eye layer count");
    if (!EnsureTargets(f.extent)) {
        return false;
    }
    // FAST bit 41: the second eye's pair of intermediates (stereo only; kept once created).
    const bool per_eye = f.per_eye_targets && f.layers == 2;
    if (per_eye) {
        const bool create = !edges_eye1.image || edges_eye1.extent != f.extent;
        if (!EnsureTarget(edges_eye1, f.extent, edges_format, "vr_smaa edges (eye 1)") ||
            !EnsureTarget(weights_eye1, f.extent, vk::Format::eR8G8B8A8Unorm,
                          "vr_smaa weights (eye 1)")) {
            return false;
        }
        if (create) {
            LOG_INFO(Render_Vulkan,
                     "VR smaa: {}x{} second-eye intermediates (FAST bit 41: each pass for both "
                     "eyes, 3 barriers per frame)",
                     f.extent.width, f.extent.height);
        }
    }
    // Preflight both eyes before recording anything. Cache handles, not pointers into views.
    std::array<std::array<vk::ImageView, 3>, 2> eye_views{};
    for (u32 eye = 0; eye < f.layers; ++eye) {
        const auto* src = ViewsOf(f.src, f.src_uid, f.src_format, true, eye);
        if (!src)
            return refuse(1u << 3, "no views of the FXAA texture");
        eye_views[eye][0] = src->unorm;
        eye_views[eye][1] = src->srgb;
        const auto* dst = ViewsOf(f.dst, f.dst_uid, f.dst_format, false, eye);
        if (!dst)
            return refuse(1u << 4, "no storage view of the FXAA target");
        eye_views[eye][2] = dst->unorm;
    }

    scheduler.GpuWorkBegin(TimerKey, f.extent.width, f.extent.height); // VR_GPU_PASSES only
    GpuPassProfiler::MarkAt(scheduler, TimerKey, GpuPassProfiler::Group::Aa,
                            "VR SMAA (in place of the game's FXAA, both eyes)");
    if (!uploaded) {
        Upload(cmdbuf);
        if (!ready) {
            scheduler.GpuWorkEnd();
            return false;
        }
    }
    // Identical for both eyes and all three passes.
    const SmaaPush push{
        .rt_metrics = {1.0f / static_cast<float>(f.extent.width),
                       1.0f / static_cast<float>(f.extent.height),
                       static_cast<float>(f.extent.width), static_cast<float>(f.extent.height)},
        .size = {static_cast<s32>(f.extent.width), static_cast<s32>(f.extent.height)},
        // The bytes' order (B G R A formats) and the game's component swap both move red.
        .flags = (src_order == 1) != f.swap_rb ? 1u : 0u,
    };
    const u32 gx = DivCeil(f.extent.width, Group);
    const u32 gy = DivCeil(f.extent.height, Group);
    const auto sampled = [&](vk::ImageView view, vk::ImageLayout layout) {
        return vk::DescriptorImageInfo{
            .sampler = *sampler,
            .imageView = view,
            .imageLayout = layout,
        };
    };
    // FAST_GPU_SMAA_TRIM's weights and blend passes for this frame; `full` are today's (and a
    // trim whose pipeline did not build). Both are the same for both eyes.
    struct Passes {
        vk::Pipeline weights;
        u32 weights_x; // its groups (SMAA_COMPACT: 16x16 ones)
        u32 weights_y;
        vk::Pipeline blend;
    };
    const Passes full{*weights_pipeline, gx, gy, *blend_pipeline};
    const bool keep = (g_smaa_trim & SmaaTrim::Keep) != 0 && blend_keep_pipeline && !f.dst_cleared;
    const bool compact = (g_smaa_trim & SmaaTrim::Compact) != 0 && weights_compact_pipeline;
    const Passes trimmed{
        .weights = compact ? *weights_compact_pipeline : *weights_pipeline,
        .weights_x = compact ? DivCeil(f.extent.width, SmaaTrim::CompactGroup) : gx,
        .weights_y = compact ? DivCeil(f.extent.height, SmaaTrim::CompactGroup) : gy,
        .blend = keep ? *blend_keep_pipeline : *blend_pipeline,
    };
    // One pass for one eye, `x` by `y` groups. `bind`: the pipeline and the push constants first
    // (every pass of the eye-major order; the first eye's dispatch of a pass in bit 41's order,
    // the second eye's reusing both: all the pipelines share one layout and only the push
    // descriptors change).
    const auto pass = [&](vk::Pipeline pipeline,
                          std::initializer_list<std::pair<u32, vk::DescriptorImageInfo>> in,
                          vk::ImageView out, bool bind, u32 x, u32 y) {
        std::array<vk::DescriptorImageInfo, 7> infos{};
        std::array<vk::WriteDescriptorSet, 7> writes{};
        u32 n = 0;
        for (const auto& [binding, info] : in) {
            infos[n] = info;
            writes[n] = {
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &infos[n],
            };
            ++n;
        }
        infos[n] = {.imageView = out, .imageLayout = vk::ImageLayout::eGeneral};
        writes[n] = {
            .dstBinding = 6,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[n],
        };
        ++n;
        if (bind) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        }
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0,
                                    vk::ArrayProxy<const vk::WriteDescriptorSet>{n, writes.data()});
        if (bind) {
            ScratchPush::Invalidate();
            cmdbuf.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                 &push);
        }
        cmdbuf.dispatch(x, y, 1);
    };
    const auto barrier = [&](std::initializer_list<vk::ImageMemoryBarrier2> list) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(list.size()),
            .pImageMemoryBarriers = list.begin(),
        });
        ++barrier_calls;
    };
    // The eye-major order (bit 41 off, mono, and bit 41's verify reference): the intermediate
    // targets are reused between eyes; the compute barriers order the previous eye's readers
    // before overwriting them. 3 barriers per eye.
    const auto eye_major = [&](const Passes& ps) {
        const vk::Image edges_image = edges.image;
        const vk::Image weights_image = weights.image;
        for (u32 eye = 0; eye < f.layers; ++eye) {
            const auto [src_unorm, src_srgb, dst_view] = eye_views[eye];
            // 1. Luma edges (B, gamma space) -> edges. The edges' last readers (the previous
            //    eye's or frame's weights pass) are done.
            barrier({ComputeBarrier(edges_image, edges.fresh)});
            edges.fresh = false;
            pass(*edges_pipeline,
                 {{0, sampled(src_unorm, vk::ImageLayout::eShaderReadOnlyOptimal)}}, *edges.view,
                 true, gx, gy);
            // 2. Blending weights.
            barrier(
                {ComputeBarrier(edges_image, false), ComputeBarrier(weights_image, weights.fresh)});
            weights.fresh = false;
            pass(ps.weights,
                 {{2, sampled(*edges.view, vk::ImageLayout::eGeneral)},
                  {3, sampled(*area_view, vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {4, sampled(*search_view, vk::ImageLayout::eShaderReadOnlyOptimal)}},
                 *weights.view, true, ps.weights_x, ps.weights_y);
            // 3. Neighbourhood blending: B (linear light) + weights -> A.
            barrier({ComputeBarrier(weights_image, false)});
            pass(ps.blend,
                 {{0, sampled(src_unorm, vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {1, sampled(src_srgb, vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {5, sampled(*weights.view, vk::ImageLayout::eGeneral)}},
                 dst_view, true, gx, gy);
            if (frame == 1) {
                LOG_INFO(Render_Vulkan,
                         "VR smaa: eye {} of {} first frame {}x{} ({} -> {}, colour as {}, {}x{} "
                         "groups per pass)",
                         eye, f.layers, f.extent.width, f.extent.height,
                         vk::to_string(f.src_format), vk::to_string(f.dst_format),
                         push.flags != 0 ? "B G R" : "R G B", gx, gy);
            }
        }
    };
    // FAST bit 41's pass-major order: each pass for both eyes between the same two barriers,
    // through per-eye intermediates (3 barriers per frame instead of 6; the two eyes' dispatches
    // of a pass may overlap on the GPU). Bit-exact with the eye-major order: every dispatch runs
    // the same pipeline with the same push constants, the same views of B, A and the lookup
    // textures and the same sampler, and eye 1's intermediates are created like eye 0's (format,
    // extent, usage, tiling). Each pass writes every texel inside the extent before the next pass
    // reads it (no edge stores a zero edge, every weight is stored; the clamp sampler never reads
    // outside the extent), so no result depends on which image held an intermediate or on its
    // previous contents. Per eye, the barriers keep the eye-major order's RAW (edges -> weights
    // -> blend) and WAR (the previous frame's readers) dependencies; the eyes share no image the
    // passes write (A's layers are separate views). Checked on the GPU by
    // FAST_SMAA_RECORDER_VERIFY.
    const auto pass_major = [&](const Passes& ps) {
        const vk::Image edges0 = edges.image;
        const vk::Image edges1 = edges_eye1.image;
        const vk::Image weights0 = weights.image;
        const vk::Image weights1 = weights_eye1.image;
        const std::array<vk::ImageView, 2> edge_views{*edges.view, *edges_eye1.view};
        const std::array<vk::ImageView, 2> weight_views{*weights.view, *weights_eye1.view};
        // 1. Luma edges, both eyes.
        barrier({ComputeBarrier(edges0, edges.fresh), ComputeBarrier(edges1, edges_eye1.fresh)});
        edges.fresh = false;
        edges_eye1.fresh = false;
        for (u32 eye = 0; eye < 2; ++eye) {
            pass(*edges_pipeline,
                 {{0, sampled(eye_views[eye][0], vk::ImageLayout::eShaderReadOnlyOptimal)}},
                 edge_views[eye], eye == 0, gx, gy);
        }
        // 2. Blending weights, both eyes.
        barrier({ComputeBarrier(edges0, false), ComputeBarrier(edges1, false),
                 ComputeBarrier(weights0, weights.fresh),
                 ComputeBarrier(weights1, weights_eye1.fresh)});
        weights.fresh = false;
        weights_eye1.fresh = false;
        for (u32 eye = 0; eye < 2; ++eye) {
            pass(ps.weights,
                 {{2, sampled(edge_views[eye], vk::ImageLayout::eGeneral)},
                  {3, sampled(*area_view, vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {4, sampled(*search_view, vk::ImageLayout::eShaderReadOnlyOptimal)}},
                 weight_views[eye], eye == 0, ps.weights_x, ps.weights_y);
        }
        // 3. Neighbourhood blending, both eyes: B's layer + weights -> A's layer.
        barrier({ComputeBarrier(weights0, false), ComputeBarrier(weights1, false)});
        for (u32 eye = 0; eye < 2; ++eye) {
            pass(ps.blend,
                 {{0, sampled(eye_views[eye][0], vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {1, sampled(eye_views[eye][1], vk::ImageLayout::eShaderReadOnlyOptimal)},
                  {5, sampled(weight_views[eye], vk::ImageLayout::eGeneral)}},
                 eye_views[eye][2], eye == 0, gx, gy);
            if (frame == 1) {
                LOG_INFO(Render_Vulkan,
                         "VR smaa: eye {} of {} first frame {}x{} ({} -> {}, colour as {}, {}x{} "
                         "groups per pass, pass-major)",
                         eye, f.layers, f.extent.width, f.extent.height,
                         vk::to_string(f.src_format), vk::to_string(f.dst_format),
                         push.flags != 0 ? "B G R" : "R G B", gx, gy);
            }
        }
    };
    // The frame's passes: bit 41's pass-major order for a stereo pair, else the eye-major one,
    // with FAST_GPU_SMAA_TRIM's trims (none: `full`).
    const bool trim = keep || compact;
    if (per_eye) {
        pass_major(trimmed);
        ++per_eye_frames;
    } else {
        eye_major(trimmed);
    }
    // At most one check per frame. FAST_GPU_SMAA_TRIM_VERIFY=N, every Nth trimmed frame: the
    // full passes again in the same order (A ends with today's pixels). Else
    // FAST_SMAA_RECORDER_VERIFY=N, every Nth stereo pair: the eye-major order with the same
    // trims (Keep's A then keeps the same bytes either way: the pixels without a weight are not
    // stored in either order). Either way A (both layers) is copied to a readback buffer before
    // and after the second run (the passes only read B and the lookup textures, which nothing
    // changed, and only write A), and the two copies are compared on the scheduler's
    // priority-operation thread once the GPU has finished the command buffer.
    bool trim_check = false;
    if (trim) {
        ++trim_frames;
        trim_check = g_smaa_trim_verify != 0 && ++trim_verify_tick % g_smaa_trim_verify == 0;
        ReportTrim(keep, compact);
    }
    if (!trim_check) {
        const u64 every = per_eye ? SmaaVerifyEvery() : 0;
        if (every == 0 || ++verify_tick % every != 0) {
            scheduler.GpuWorkEnd();
            return true;
        }
    }
    const VkDeviceSize eye_bytes = VkDeviceSize{f.extent.width} * f.extent.height * 4u;
    const VkDeviceSize bytes = eye_bytes * f.layers;
    const VkBufferCreateInfo readback_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo readback_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    const VmaAllocator allocator = instance.GetAllocator();
    VkBuffer readback{};
    VmaAllocation allocation{};
    VmaAllocationInfo mapped{};
    const VkResult created = vmaCreateBuffer(allocator, &readback_info, &readback_alloc, &readback,
                                             &allocation, &mapped);
    if (created != VK_SUCCESS || mapped.pMappedData == nullptr) {
        if (created == VK_SUCCESS) {
            vmaDestroyBuffer(allocator, readback, allocation);
        }
        if ((warned & (1u << 6)) == 0) {
            warned |= 1u << 6;
            LOG_WARNING(Render_Vulkan,
                        "FAST smaa_recorder verify: no readback buffer; not checked");
        }
        scheduler.GpuWorkEnd();
        return true;
    }
    const vk::ImageSubresourceRange a_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = f.layers,
    };
    // A's compute writes -> the copy; then the copy -> A's next writer (the eye-major passes, or
    // after the last copy whatever the texture cache orders after compute: its next barrier's
    // source stages include the compute stage this chains through), and the last copy -> host.
    const auto read_a = [&](VkDeviceSize offset, bool last) {
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = f.dst,
            .subresourceRange = a_range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_copy,
        });
        const vk::BufferImageCopy region{
            .bufferOffset = offset,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, f.layers},
            .imageExtent = {f.extent.width, f.extent.height, 1},
        };
        cmdbuf.copyImageToBuffer(f.dst, vk::ImageLayout::eGeneral, vk::Buffer{readback}, region);
        const vk::ImageMemoryBarrier2 after_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = f.dst,
            .subresourceRange = a_range,
        };
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = last ? 1u : 0u,
            .pMemoryBarriers = last ? &to_host : nullptr,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &after_copy,
        });
    };
    read_a(0, false);
    if (!trim_check) {
        eye_major(trimmed);
    } else if (per_eye) {
        pass_major(full);
    } else {
        eye_major(full);
    }
    read_a(bytes, true);
    if (trim_check) {
        ++trim_verify_recorded;
    } else {
        ++verify_recorded;
    }
    const u8* data = static_cast<const u8*>(mapped.pMappedData);
    const u32 row = f.extent.width * 4u;
    scheduler.DeferPriorityOperation([allocator, readback, allocation, data, bytes, eye_bytes, row,
                                      trim_check] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        const size_t n = static_cast<size_t>(bytes);
        if (trim_check) {
            // Texel for texel: the trimmed passes' A (first copy) against the full passes' A.
            const SmaaTrim::TexelDiff d = SmaaTrim::CountTexelDiffs(data, data + n, n / 4);
            if (d.first != ~u64{0}) {
                g_trim_differing.fetch_add(1, std::memory_order_relaxed);
                g_trim_color_texels.fetch_add(d.color, std::memory_order_relaxed);
                g_trim_alpha_texels.fetch_add(d.alpha_only, std::memory_order_relaxed);
                if (g_trim_error_lines.fetch_add(1, std::memory_order_relaxed) < TrimErrorLines) {
                    const u64 within = (d.first * 4) % eye_bytes;
                    const auto& a = d.first_fast;
                    const auto& b = d.first_ref;
                    LOG_ERROR(Render_Vulkan,
                              "FAST_GPU_SMAA_TRIM verify: the trimmed SMAA differs from the full "
                              "passes in {} of {} texels ({} in colour, {} in alpha only; first: "
                              "eye {} x {} y {}, bytes {:02x} {:02x} {:02x} {:02x} against {:02x} "
                              "{:02x} {:02x} {:02x}): leave the trim off",
                              d.color + d.alpha_only, n / 4, d.color, d.alpha_only,
                              (d.first * 4) / eye_bytes, (within % row) / 4, within / row, a[0],
                              a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                }
            }
            g_trim_compared.fetch_add(1, std::memory_order_relaxed);
        } else if (std::memcmp(data, data + n, n) != 0) {
            size_t first = n;
            size_t differing = 0;
            for (size_t i = 0; i < n; ++i) {
                if (data[i] != data[n + i]) {
                    if (first == n) {
                        first = i;
                    }
                    ++differing;
                }
            }
            const u64 within = first % eye_bytes;
            LOG_ERROR(Render_Vulkan,
                      "FAST smaa_recorder verify: per-eye SMAA differs from the eye-major order "
                      "in {} of {} bytes (first: eye {} x {} y {})",
                      differing, n, first / eye_bytes, (within % row) / 4, within / row);
            g_smaa_verify_mismatched.fetch_add(1, std::memory_order_relaxed);
        }
        if (!trim_check) {
            g_smaa_verify_compared.fetch_add(1, std::memory_order_relaxed);
        }
        vmaDestroyBuffer(allocator, readback, allocation);
    });
    scheduler.GpuWorkEnd();
    return true;
}

} // namespace Vulkan
