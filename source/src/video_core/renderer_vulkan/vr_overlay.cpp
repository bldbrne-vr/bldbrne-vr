#include "../../../scratchpad/dual-eye-perf/push_state_memo.h"
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_overlay.h"

#ifdef ENABLE_OPENXR

#include <algorithm>
#include <cmath>
#include <cstring>

#include <vk_mem_alloc.h>
#include <openxr/openxr_platform.h>

#include "common/logging/log.h"
#include "video_core/host_shaders/vr_overlay_frag.h"
#include "video_core/host_shaders/vr_overlay_vert.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"
#include "video_core/renderer_vulkan/vr_device_lost.h"

namespace Vulkan {

namespace O = VrOverlay;

namespace {

constexpr u64 WaitImageTimeoutNs = 1'000'000;         // 1 ms: never stall the frame thread
constexpr u64 UploadTimeoutNs = 5'000'000'000;        // mesh upload at Init
constexpr u64 ShutdownTimeoutNs = 2'000'000'000;
constexpr u64 ReuseFrames = 3; // the last image may stand in for at most this many frames

// The union of both eyes' FOV in degrees: left, right, up, down (FrameConstants::field).
std::array<float, 4> FieldOf(const std::array<XrFovf, 2>& fov) {
    constexpr float Deg = 57.2957795f;
    const XrFovf& a = fov[0];
    const XrFovf& b = fov[1];
    return {std::max(-a.angleLeft, -b.angleLeft) * Deg, std::max(a.angleRight, b.angleRight) * Deg,
            std::max(a.angleUp, b.angleUp) * Deg, std::max(-a.angleDown, -b.angleDown) * Deg};
}

O::Pose ToPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

O::Mat4 ViewProjection(const XrPosef& pose, const XrFovf& fov, float near_m) {
    return O::ProjectionReversedZ(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown,
                                  near_m) *
           O::RigidInverse(O::Mat4::FromPose(ToPose(pose)));
}

u32 PipelineIndex(O::Pass pass) {
    return static_cast<u32>(pass == O::Pass::Holdout ? O::Pass::DepthOnly : pass);
}

vk::SampleCountFlagBits PickSamples(vk::SampleCountFlags supported, u32 wanted) {
    constexpr std::array<std::pair<u32, vk::SampleCountFlagBits>, 4> counts{{
        {8, vk::SampleCountFlagBits::e8},
        {4, vk::SampleCountFlagBits::e4},
        {2, vk::SampleCountFlagBits::e2},
        {1, vk::SampleCountFlagBits::e1},
    }};
    for (const auto& [n, bit] : counts) {
        if (n <= wanted && (supported & bit)) {
            return bit;
        }
    }
    return vk::SampleCountFlagBits::e1;
}

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

} // namespace

VrOverlayRenderer::VrOverlayRenderer() = default;

VrOverlayRenderer::~VrOverlayRenderer() {
    Shutdown();
}

bool VrOverlayRenderer::Fail(std::string_view what) {
    LOG_ERROR(Render_Vulkan, "VR overlay disabled: {}", what);
    failed = true;
    return false;
}

void VrOverlayRenderer::Disable(std::string_view reason) {
    if (!failed) {
        LOG_WARNING(Render_Vulkan, "VR overlay disabled: {}", reason);
    }
    failed = true;
    recorded = false;
    want_layer = false;
}

bool VrOverlayRenderer::Init(const DeviceContext& device, const SessionContext& session,
                             const Settings& s) {
    Shutdown();
    dev = device;
    xr = session;
    settings = s;
    settings.scale = std::clamp(settings.scale, 0.5f, 1.0f);
    settings.msaa = std::clamp<u32>(settings.msaa, 1, 8);
    settings.near_m = std::clamp(settings.near_m, 0.005f, 0.2f);
    failed = false;
    if (!settings.enabled) {
        LOG_INFO(Render_Vulkan, "VR overlay off (vr.ini overlay = 0)");
        return false;
    }
    if (!dev.device || !dev.physical_device || !dev.queue || !dev.allocator || !dev.queue_mutex ||
        xr.instance == XR_NULL_HANDLE || xr.session == XR_NULL_HANDLE ||
        xr.space == XR_NULL_HANDLE) {
        return Fail("missing device or session handles");
    }
    bool ok = CreateSwapchain() && CreateTargets() && CreateFrameResources() && UploadMeshes() &&
              CreatePipelines();
    if (ok) {
        const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
        const LockTrace::Start lock_start;
        std::scoped_lock lk{*dev.queue_mutex};
        LockTrace::Hold trace{"overlay init: acquire", lock_start}; // VR_LOCK_TRACE only
        ok = AcquireImage();
    }
    xr.formats = {}; // not kept beyond Init
    if (!ok) {
        failed = true;
        Shutdown();
        failed = true;
        return false;
    }
    initialized = true;
    LOG_INFO(Render_Vulkan, "VR overlay ready: {}", Describe());
    return true;
}

bool VrOverlayRenderer::CreateSwapchain() {
    // sRGB with alpha: the shaders write linear premultiplied color.
    static constexpr std::array preferred{static_cast<s64>(VK_FORMAT_R8G8B8A8_SRGB),
                                          static_cast<s64>(VK_FORMAT_B8G8R8A8_SRGB)};
    s64 chosen = 0;
    for (const s64 f : preferred) {
        if (std::ranges::find(xr.formats, f) != xr.formats.end()) {
            chosen = f;
            break;
        }
    }
    if (chosen == 0) {
        return Fail("the runtime offers no RGBA8 / BGRA8 sRGB swapchain format");
    }
    color_format = static_cast<vk::Format>(chosen);

    const XrViewConfigurationView& view = xr.views[0];
    u32 w = static_cast<u32>(std::lround(view.recommendedImageRectWidth * settings.scale));
    u32 h = static_cast<u32>(std::lround(view.recommendedImageRectHeight * settings.scale));
    w = std::clamp<u32>(w, 64, std::max<u32>(view.maxImageRectWidth, 64));
    h = std::clamp<u32>(h, 64, std::max<u32>(view.maxImageRectHeight, 64));
    // Both eyes side by side must fit the runtime's and the device's limits.
    u32 max_w = dev.physical_device.getProperties().limits.maxFramebufferWidth;
    u32 max_h = dev.physical_device.getProperties().limits.maxFramebufferHeight;
    XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.system, &system))) {
        if (system.graphicsProperties.maxSwapchainImageWidth > 0) {
            max_w = std::min(max_w, system.graphicsProperties.maxSwapchainImageWidth);
        }
        if (system.graphicsProperties.maxSwapchainImageHeight > 0) {
            max_h = std::min(max_h, system.graphicsProperties.maxSwapchainImageHeight);
        }
    }
    const float fit = std::min({1.0f, static_cast<float>(max_w) / (2.0f * w),
                                static_cast<float>(max_h) / static_cast<float>(h)});
    if (fit < 1.0f) {
        w = std::max<u32>(static_cast<u32>(w * fit), 64);
        h = std::max<u32>(static_cast<u32>(h * fit), 64);
    }
    eye_extent = {w, h};

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = 2 * w;
    info.height = h;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (const XrResult r = xrCreateSwapchain(xr.session, &info, &swapchain); XR_FAILED(r)) {
        swapchain = XR_NULL_HANDLE;
        return Fail(fmt::format("xrCreateSwapchain {}x{} failed ({})", 2 * w, h,
                                static_cast<s32>(r)));
    }
    u32 count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    std::vector<XrSwapchainImageVulkanKHR> images(count,
                                                  {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, {}});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
        return Fail("xrEnumerateSwapchainImages failed");
    }
    for (u32 i = 0; i < count; ++i) {
        const vk::Image image{images[i].image};
        swapchain_images.push_back(image);
        auto [result, view_handle] = dev.device.createImageView({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = color_format,
            .subresourceRange = ColorRange,
        });
        if (result != vk::Result::eSuccess) {
            return Fail(fmt::format("swapchain image view: {}", vk::to_string(result)));
        }
        swapchain_views.push_back(view_handle);
        SetObjectName(dev.device, view_handle, "VR overlay swapchain view {}", i);
    }
    memory_bytes += static_cast<u64>(2 * w) * h * 4 * count;
    return true;
}

bool VrOverlayRenderer::CreateImage(ImageResource& out, vk::Format format,
                                    vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                                    vk::SampleCountFlagBits sample_count, std::string_view name) {
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {2 * eye_extent.width, eye_extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = static_cast<VkSampleCountFlagBits>(sample_count),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = static_cast<VkImageUsageFlags>(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image{};
    VmaAllocation allocation{};
    // Transient attachments live in lazily allocated memory where it exists (tilers); desktop
    // GPUs fall back to ordinary device memory.
    VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                  .usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED};
    VkResult result =
        vmaCreateImage(dev.allocator, &info, &alloc, &image, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        result = vmaCreateImage(dev.allocator, &info, &alloc, &image, &allocation, nullptr);
    }
    if (result != VK_SUCCESS) {
        return Fail(fmt::format("{}: allocation failed ({})", name, static_cast<s32>(result)));
    }
    out.image = vk::Image{image};
    out.allocation = allocation;
    VmaAllocationInfo alloc_info{};
    vmaGetAllocationInfo(dev.allocator, allocation, &alloc_info);
    memory_bytes += alloc_info.size;
    auto [view_result, view] = dev.device.createImageView({
        .image = out.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });
    if (view_result != vk::Result::eSuccess) {
        return Fail(fmt::format("{}: view failed ({})", name, vk::to_string(view_result)));
    }
    out.view = view;
    SetObjectName(dev.device, out.image, name);
    return true;
}

void VrOverlayRenderer::DestroyImage(ImageResource& image) {
    if (image.view) {
        dev.device.destroyImageView(image.view);
    }
    if (image.image) {
        vmaDestroyImage(dev.allocator, static_cast<VkImage>(image.image), image.allocation);
    }
    image = {};
}

bool VrOverlayRenderer::CreateBuffer(BufferResource& out, vk::DeviceSize size,
                                     vk::BufferUsageFlags usage, bool host_visible) {
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = static_cast<VkBufferUsageFlags>(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc{};
    if (host_visible) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
    } else {
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    if (const VkResult r =
            vmaCreateBuffer(dev.allocator, &info, &alloc, &buffer, &allocation, &alloc_info);
        r != VK_SUCCESS) {
        return Fail(fmt::format("buffer allocation failed ({})", static_cast<s32>(r)));
    }
    out.buffer = vk::Buffer{buffer};
    out.allocation = allocation;
    out.mapped = alloc_info.pMappedData;
    if (host_visible && out.mapped == nullptr) {
        return Fail("host buffer is not mapped");
    }
    return true;
}

void VrOverlayRenderer::DestroyBuffer(BufferResource& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(dev.allocator, static_cast<VkBuffer>(buffer.buffer), buffer.allocation);
    }
    buffer = {};
}

bool VrOverlayRenderer::CreateTargets() {
    const auto limits = dev.physical_device.getProperties().limits;
    // Depth: D32 float (reversed Z) when it is a depth attachment format, else D16.
    for (const vk::Format f : {vk::Format::eD32Sfloat, vk::Format::eD16Unorm}) {
        const auto props = dev.physical_device.getFormatProperties(f);
        if (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
            depth_format = f;
            break;
        }
    }
    if (depth_format == vk::Format::eUndefined) {
        return Fail("no depth attachment format");
    }
    vk::SampleCountFlags supported =
        limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
    const auto image_samples = [&](vk::Format format, vk::ImageUsageFlags usage) {
        auto [result, props] = dev.physical_device.getImageFormatProperties(
            format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, usage, {});
        return result == vk::Result::eSuccess ? props.sampleCounts
                                              : vk::SampleCountFlags{vk::SampleCountFlagBits::e1};
    };
    supported &= image_samples(color_format, vk::ImageUsageFlagBits::eColorAttachment |
                                                 vk::ImageUsageFlagBits::eTransientAttachment);
    supported &=
        image_samples(depth_format, vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                        vk::ImageUsageFlagBits::eTransientAttachment);
    samples = PickSamples(supported, settings.msaa);
    if (samples != vk::SampleCountFlagBits::e1 &&
        !CreateImage(msaa_color, color_format,
                     vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eTransientAttachment,
                     vk::ImageAspectFlagBits::eColor, samples, "VR overlay MSAA color")) {
        return false;
    }
    return CreateImage(depth, depth_format,
                       vk::ImageUsageFlagBits::eDepthStencilAttachment |
                           vk::ImageUsageFlagBits::eTransientAttachment,
                       vk::ImageAspectFlagBits::eDepth, samples, "VR overlay depth");
}

bool VrOverlayRenderer::CreateFrameResources() {
    auto [pool_result, pool] = dev.device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                 vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = dev.queue_family,
    });
    if (pool_result != vk::Result::eSuccess) {
        return Fail("command pool");
    }
    command_pool = pool;
    auto [cmd_result, cmds] = dev.device.allocateCommandBuffers({
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 2,
    });
    if (cmd_result != vk::Result::eSuccess || cmds.size() != 2) {
        return Fail("command buffers");
    }
    for (u32 i = 0; i < 2; ++i) {
        slots[i].cmd = cmds[i];
        auto [fence_result, fence] = dev.device.createFence({});
        if (fence_result != vk::Result::eSuccess) {
            return Fail("fence");
        }
        slots[i].fence = fence;
    }

    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
    };
    auto [layout_result, layout] = dev.device.createDescriptorSetLayout({
        .bindingCount = 1,
        .pBindings = &binding,
    });
    if (layout_result != vk::Result::eSuccess) {
        return Fail("descriptor set layout");
    }
    set_layout = layout;
    const vk::DescriptorPoolSize pool_size{.type = vk::DescriptorType::eUniformBuffer,
                                           .descriptorCount = 2};
    auto [dp_result, dp] = dev.device.createDescriptorPool({
        .maxSets = 2,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    if (dp_result != vk::Result::eSuccess) {
        return Fail("descriptor pool");
    }
    descriptor_pool = dp;
    const std::array layouts{set_layout, set_layout};
    auto [sets_result, sets] = dev.device.allocateDescriptorSets({
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = layouts.data(),
    });
    if (sets_result != vk::Result::eSuccess || sets.size() != 2) {
        return Fail("descriptor sets");
    }

    const auto limits = dev.physical_device.getProperties().limits;
    const vk::DeviceSize align = std::max<vk::DeviceSize>(limits.minUniformBufferOffsetAlignment, 16);
    uniform_stride = (sizeof(O::FrameConstants) + align - 1) / align * align;
    if (!CreateBuffer(uniforms, 2 * uniform_stride, vk::BufferUsageFlagBits::eUniformBuffer,
                      true)) {
        return false;
    }
    for (u32 i = 0; i < 2; ++i) {
        slots[i].set = sets[i];
        const vk::DescriptorBufferInfo buffer_info{
            .buffer = uniforms.buffer,
            .offset = i * uniform_stride,
            .range = sizeof(O::FrameConstants),
        };
        const vk::WriteDescriptorSet write{
            .dstSet = sets[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &buffer_info,
        };
        dev.device.updateDescriptorSets(write, {});
    }

    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(O::DrawConstants),
    };
    auto [pl_result, pl] = dev.device.createPipelineLayout({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (pl_result != vk::Result::eSuccess) {
        return Fail("pipeline layout");
    }
    pipeline_layout = pl;

    // GPU time per image (optional).
    const auto families = dev.physical_device.getQueueFamilyProperties();
    if (dev.queue_family < families.size() &&
        families[dev.queue_family].timestampValidBits > 0 && limits.timestampPeriod > 0.0f) {
        auto [qp_result, qp] = dev.device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = 4,
        });
        if (qp_result == vk::Result::eSuccess) {
            query_pool = qp;
            timestamp_period_ns = limits.timestampPeriod;
            timestamp_mask = families[dev.queue_family].timestampValidBits >= 64
                                 ? ~u64{0}
                                 : (u64{1} << families[dev.queue_family].timestampValidBits) - 1;
        }
    }
    return true;
}

bool VrOverlayRenderer::UploadMeshes() {
    const O::MeshLibrary library = O::BuildLibrary();
    ranges = library.ranges;
    const vk::DeviceSize vb_size = library.vertices.size() * sizeof(O::Vertex);
    const vk::DeviceSize ib_size = library.indices.size() * sizeof(u32);
    if (!CreateBuffer(vertices, vb_size,
                      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                      false) ||
        !CreateBuffer(indices, ib_size,
                      vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                      false)) {
        return false;
    }
    BufferResource staging{};
    if (!CreateBuffer(staging, vb_size + ib_size, vk::BufferUsageFlagBits::eTransferSrc, true)) {
        return false;
    }
    std::memcpy(staging.mapped, library.vertices.data(), vb_size);
    std::memcpy(static_cast<u8*>(staging.mapped) + vb_size, library.indices.data(), ib_size);
    vmaFlushAllocation(dev.allocator, staging.allocation, 0, VK_WHOLE_SIZE);

    // A one-time command buffer (slot 0's, not yet in use) and fence.
    const vk::CommandBuffer cmd = slots[0].cmd;
    bool ok = cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
              vk::Result::eSuccess;
    if (ok) {
        cmd.copyBuffer(staging.buffer, vertices.buffer,
                       vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = vb_size});
        cmd.copyBuffer(staging.buffer, indices.buffer,
                       vk::BufferCopy{.srcOffset = vb_size, .dstOffset = 0, .size = ib_size});
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput |
                            vk::PipelineStageFlagBits2::eIndexInput,
            .dstAccessMask =
                vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        ok = cmd.end() == vk::Result::eSuccess;
    }
    if (ok) {
        const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
        const LockTrace::Start lock_start;
        std::scoped_lock lk{*dev.queue_mutex};
        LockTrace::Hold trace{"overlay mesh upload", lock_start}; // VR_LOCK_TRACE only
        ok = dev.queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd},
                              slots[0].fence) == vk::Result::eSuccess;
    }
    if (!ok) {
        DestroyBuffer(staging);
        return Fail("mesh upload submit");
    }
    if (dev.device.waitForFences(slots[0].fence, VK_TRUE, UploadTimeoutNs) !=
        vk::Result::eSuccess) {
        // The copy may still be running: leak the staging buffer rather than free it under it.
        slots[0].pending = true;
        return Fail("mesh upload did not finish");
    }
    DestroyBuffer(staging);
    if (dev.device.resetFences(slots[0].fence) != vk::Result::eSuccess) {
        return Fail("fence reset");
    }
    LOG_INFO(Render_Vulkan, "VR overlay meshes: {} vertices, {} triangles ({} KB)",
             library.vertices.size(), library.indices.size() / 3, (vb_size + ib_size) / 1024);
    return true;
}

bool VrOverlayRenderer::CreatePipelines() {
    const vk::ShaderModule vs =
        Compile(HostShaders::VR_OVERLAY_VERT, vk::ShaderStageFlagBits::eVertex, dev.device);
    const vk::ShaderModule fs =
        Compile(HostShaders::VR_OVERLAY_FRAG, vk::ShaderStageFlagBits::eFragment, dev.device);
    if (!vs || !fs) {
        if (vs) {
            dev.device.destroyShaderModule(vs);
        }
        if (fs) {
            dev.device.destroyShaderModule(fs);
        }
        return Fail("shader compilation");
    }
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs,
            .pName = "main",
        },
    };
    const vk::VertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(O::Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
    const std::array attributes{
        vk::VertexInputAttributeDescription{.location = 0,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32B32Sfloat,
                                            .offset = offsetof(O::Vertex, position)},
        vk::VertexInputAttributeDescription{.location = 1,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32B32Sfloat,
                                            .offset = offsetof(O::Vertex, normal)},
        vk::VertexInputAttributeDescription{.location = 2,
                                            .binding = 0,
                                            .format = vk::Format::eR32G32Sfloat,
                                            .offset = offsetof(O::Vertex, aux)},
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = samples,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = depth_format,
    };
    constexpr vk::ColorComponentFlags rgba =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    bool ok = true;
    for (u32 p = 0; p < O::PassCount && ok; ++p) {
        const auto pass = static_cast<O::Pass>(p);
        if (pass == O::Pass::Holdout) {
            continue; // shares DepthOnly
        }
        vk::CullModeFlags cull = vk::CullModeFlagBits::eBack;
        bool depth_write = false;
        vk::CompareOp compare = vk::CompareOp::eGreaterOrEqual;
        vk::PipelineColorBlendAttachmentState blend{.blendEnable = false, .colorWriteMask = rgba};
        const vk::PipelineColorBlendAttachmentState premultiplied{
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOne,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = rgba,
        };
        switch (pass) {
        case O::Pass::Opaque:
            depth_write = true;
            break;
        case O::Pass::Liquid:
            cull = vk::CullModeFlagBits::eNone;
            depth_write = true;
            blend = premultiplied;
            break;
        case O::Pass::DepthOnly:
            depth_write = true;
            blend.colorWriteMask = {};
            break;
        case O::Pass::TransEqual:
            compare = vk::CompareOp::eEqual;
            blend = premultiplied;
            break;
        case O::Pass::GlassBack:
            cull = vk::CullModeFlagBits::eFront;
            blend = premultiplied;
            break;
        case O::Pass::GlassFront:
            blend = premultiplied;
            break;
        case O::Pass::Screen:
            cull = vk::CullModeFlagBits::eNone;
            compare = vk::CompareOp::eAlways;
            blend = premultiplied;
            break;
        case O::Pass::Additive:
            cull = vk::CullModeFlagBits::eFront;
            blend = {
                .blendEnable = true,
                .srcColorBlendFactor = vk::BlendFactor::eOne,
                .dstColorBlendFactor = vk::BlendFactor::eOne,
                .colorBlendOp = vk::BlendOp::eAdd,
                .srcAlphaBlendFactor =
                    xr.additive_alpha ? vk::BlendFactor::eOne : vk::BlendFactor::eZero,
                .dstAlphaBlendFactor = vk::BlendFactor::eOne,
                .alphaBlendOp = vk::BlendOp::eAdd,
                .colorWriteMask = rgba,
            };
            break;
        default:
            break;
        }
        const vk::PipelineRasterizationStateCreateInfo raster{
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = cull,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };
        const vk::PipelineDepthStencilStateCreateInfo depth_state{
            .depthTestEnable = true,
            .depthWriteEnable = depth_write,
            .depthCompareOp = compare,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };
        const vk::PipelineColorBlendStateCreateInfo color_blend{
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &blend,
        };
        const vk::GraphicsPipelineCreateInfo info{
            .pNext = &rendering,
            .stageCount = static_cast<u32>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_state,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic,
            .layout = pipeline_layout,
        };
        auto [result, pipeline] = dev.device.createGraphicsPipeline({}, info);
        if (result != vk::Result::eSuccess) {
            ok = Fail(fmt::format("pipeline {} ({})", p, vk::to_string(result)));
            break;
        }
        pipelines[p] = pipeline;
        SetObjectName(dev.device, pipeline, "VR overlay pass {}", p);
    }
    if (ok) {
        pipelines[static_cast<u32>(O::Pass::Holdout)] =
            pipelines[static_cast<u32>(O::Pass::DepthOnly)];
    }
    dev.device.destroyShaderModule(vs);
    dev.device.destroyShaderModule(fs);
    return ok;
}

// Queue lock held.
bool VrOverlayRenderer::AcquireImage() {
    const XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    u32 index = 0;
    if (const XrResult r = xrAcquireSwapchainImage(swapchain, &info, &index); XR_FAILED(r)) {
        Disable(fmt::format("xrAcquireSwapchainImage failed ({})", static_cast<s32>(r)));
        return false;
    }
    if (index >= swapchain_views.size()) {
        Disable("swapchain image index out of range");
        return false;
    }
    image_index = index;
    acquired = true;
    waited = false;
    return true;
}

void VrOverlayRenderer::Shutdown() {
    if (dev.device) {
        // Only the overlay's own work (vkDeviceWaitIdle would need every queue's lock).
        std::vector<vk::Fence> busy;
        for (const Slot& s : slots) {
            if (s.fence && s.pending) {
                busy.push_back(s.fence);
            }
        }
        if (!busy.empty() &&
            dev.device.waitForFences(busy, VK_TRUE, ShutdownTimeoutNs) != vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "VR overlay shutdown: GPU work did not finish");
        }
        for (u32 p = 0; p < O::PassCount; ++p) {
            if (pipelines[p] && p != static_cast<u32>(O::Pass::Holdout)) {
                dev.device.destroyPipeline(pipelines[p]);
            }
        }
        if (pipeline_layout) {
            dev.device.destroyPipelineLayout(pipeline_layout);
        }
        if (descriptor_pool) {
            dev.device.destroyDescriptorPool(descriptor_pool);
        }
        if (set_layout) {
            dev.device.destroyDescriptorSetLayout(set_layout);
        }
        if (query_pool) {
            dev.device.destroyQueryPool(query_pool);
        }
        for (Slot& s : slots) {
            if (s.fence) {
                dev.device.destroyFence(s.fence);
            }
        }
        if (command_pool) {
            dev.device.destroyCommandPool(command_pool);
        }
        DestroyBuffer(uniforms);
        DestroyBuffer(vertices);
        DestroyBuffer(indices);
        DestroyImage(msaa_color);
        DestroyImage(depth);
        for (const vk::ImageView view : swapchain_views) {
            dev.device.destroyImageView(view);
        }
    }
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
    }
    swapchain = XR_NULL_HANDLE;
    swapchain_images.clear();
    swapchain_views.clear();
    pipelines = {};
    pipeline_layout = vk::PipelineLayout{};
    descriptor_pool = vk::DescriptorPool{};
    set_layout = vk::DescriptorSetLayout{};
    query_pool = vk::QueryPool{};
    command_pool = vk::CommandPool{};
    slots = {};
    slot_index = 0;
    acquired = waited = has_image = false;
    recorded = want_layer = frame_open = false;
    initialized = false;
    memory_bytes = 0;
    items.clear();
}

bool VrOverlayRenderer::LocateEyes(XrTime display_time, EyeViews& out) const {
    if (!initialized || display_time <= 0) {
        return false;
    }
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = display_time;
    info.space = xr.space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    u32 count = 0;
    if (XR_FAILED(xrLocateViews(xr.session, &info, &state, 2, &count, views.data())) ||
        count != 2) {
        return false;
    }
    constexpr XrViewStateFlags needed =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return false;
    }
    for (u32 i = 0; i < 2; ++i) {
        out.pose[i] = views[i].pose;
        out.fov[i] = views[i].fov;
    }
    return true;
}

std::array<float, 4> VrOverlayRenderer::FieldDeg() const {
    if (!views_valid) {
        return {};
    }
    return FieldOf(pending.fov);
}

bool VrOverlayRenderer::HeadPose(O::Pose& head) const {
    if (!frame_open || !views_valid) {
        return false;
    }
    const O::Pose a = ToPose(pending.pose[0]);
    O::Pose b = ToPose(pending.pose[1]);
    const float dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
                      a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
    if (dot < 0.0f) {
        b.orientation = {-b.orientation.x, -b.orientation.y, -b.orientation.z, -b.orientation.w};
    }
    O::Quat q{a.orientation.x + b.orientation.x, a.orientation.y + b.orientation.y,
              a.orientation.z + b.orientation.z, a.orientation.w + b.orientation.w};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f)) {
        return false;
    }
    head.orientation = {q.x / n, q.y / n, q.z / n, q.w / n};
    head.position = (a.position + b.position) * 0.5f;
    return true;
}

void VrOverlayRenderer::BeginFrame(const EyeViews& views, bool valid, bool should_render) {
    ++frame;
    items.clear();
    recorded = false;
    want_layer = false;
    lighting_set = false;
    frame_open = IsActive() && should_render;
    views_valid = valid;
    if (valid) {
        pending = views;
    }
}

void VrOverlayRenderer::BeginFrame(XrTime display_time, bool should_render) {
    EyeViews views{};
    const bool valid = IsActive() && should_render && LocateEyes(display_time, views);
    if (valid && stereo_match.baseline_m > 0.0f) {
        // The game layer's eye distance: both eyes moved along the line between them, about its
        // midpoint (their orientations and FOVs stay the runtime's).
        XrVector3f& a = views.pose[0].position;
        XrVector3f& b = views.pose[1].position;
        const O::Vec3 left{a.x, a.y, a.z};
        const O::Vec3 d = O::Vec3{b.x, b.y, b.z} - left;
        const float distance = O::Length(d);
        if (distance > 1e-4f) {
            const O::Vec3 centre = left + d * 0.5f;
            const O::Vec3 half = d * (0.5f * stereo_match.baseline_m / distance);
            const O::Vec3 l = centre - half;
            const O::Vec3 r = centre + half;
            a = {l.x, l.y, l.z};
            b = {r.x, r.y, r.z};
        }
    }
    BeginFrame(views, valid, should_render);
}

void VrOverlayRenderer::SetLighting(const O::Lighting& l) {
    lighting = l;
    lighting_set = true;
}

void VrOverlayRenderer::Draw(const O::DrawItem& item) {
    if (frame_open) {
        items.push_back(item);
    }
}

void VrOverlayRenderer::Draw(O::MeshId mesh, O::Pass pass, O::Shade shade,
                             const O::Mat4& world_from_model, const O::Material& material) {
    Draw(O::DrawItem{mesh, pass, shade, world_from_model, material});
}

bool VrOverlayRenderer::CanReuse() const {
    return has_image && frame - shown_frame <= ReuseFrames;
}

void VrOverlayRenderer::ReadTimestamps(u32 slot) {
    if (!query_pool) {
        return;
    }
    std::array<u64, 2> t{};
    if (dev.device.getQueryPoolResults(query_pool, slot * 2, 2, sizeof(t), t.data(), sizeof(u64),
                                       vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
        const u64 ticks = (t[1] - t[0]) & timestamp_mask;
        stats.gpu_ms += static_cast<double>(ticks) * timestamp_period_ns * 1e-6;
        ++stats.gpu_samples;
    }
}

void VrOverlayRenderer::EndFrame() {
    recorded = false;
    want_layer = false;
    if (!frame_open) {
        return;
    }
    frame_open = false;
    if (items.empty()) {
        return; // nothing to show: no layer, no GPU work
    }
    ++stats.frames;
    if (!views_valid) {
        ++stats.reused;
        want_layer = CanReuse();
        return;
    }
    Slot& slot = slots[slot_index];
    if (slot.pending) {
        const vk::Result status = dev.device.getFenceStatus(slot.fence);
        if (status == vk::Result::eNotReady) {
            // The image of two frames ago is still rendering: show the last one again.
            ++stats.gpu_late;
            ++stats.reused;
            want_layer = CanReuse();
            return;
        }
        if (status != vk::Result::eSuccess) {
            VrDeviceLost::Note(status, "the VR overlay's fence");
            Disable(fmt::format("fence status {}", vk::to_string(status)));
            return;
        }
        ReadTimestamps(slot_index);
        slot.pending = false;
    }
    if (!acquired) {
        ++stats.reused;
        want_layer = CanReuse();
        return;
    }
    if (!waited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = WaitImageTimeoutNs;
        const XrResult r = xrWaitSwapchainImage(swapchain, &wait);
        if (r == XR_TIMEOUT_EXPIRED) {
            ++stats.wait_timeouts;
            ++stats.reused;
            want_layer = CanReuse();
            return; // still acquired: waited again next frame, never released unwaited
        }
        if (XR_FAILED(r)) {
            Disable(fmt::format("xrWaitSwapchainImage failed ({})", static_cast<s32>(r)));
            return;
        }
        waited = true;
    }

    O::SortDrawItems(items);
    std::array<O::Mat4, 2> view_proj;
    std::array<O::Vec3, 2> eye_pos;
    for (u32 i = 0; i < 2; ++i) {
        view_proj[i] = ViewProjection(pending.pose[i], pending.fov[i], settings.near_m);
        const XrVector3f& p = pending.pose[i].position;
        eye_pos[i] = {p.x, p.y, p.z};
    }
    // DIBR near field (Record): the eye line, its midpoint and the view direction of this frame.
    const O::Vec3 eye_line = eye_pos[1] - eye_pos[0];
    knee_half_m = 0.5f * O::Length(eye_line);
    knee_active = O::NearDisparityScale(0.01f, stereo_match.knee_m, stereo_match.min_m) < 1.0f &&
                  knee_half_m > 1e-4f;
    if (knee_active) {
        knee_centre = eye_pos[0] + eye_line * 0.5f;
        knee_right = eye_line * (0.5f / knee_half_m);
        knee_forward = O::Rotate(ToPose(pending.pose[0]).orientation, O::Vec3{0.0f, 0.0f, -1.0f});
    }
    const O::Lighting light =
        lighting_set ? lighting
                     : O::DefaultLighting(O::HeadYaw(ToPose(pending.pose[0]).orientation),
                                          settings.exposure);
    O::FrameConstants constants = O::MakeFrameConstants(view_proj, eye_pos, light);
    // What the headset shows (the fitted vignettes): the union of the eyes' located FOV.
    {
        const std::array<float, 4> f = FieldOf(pending.fov);
        constants.field = {f[0], f[1], f[2], f[3]};
    }
    std::memcpy(static_cast<u8*>(uniforms.mapped) + slot_index * uniform_stride, &constants,
                sizeof(constants));
    vmaFlushAllocation(dev.allocator, uniforms.allocation, slot_index * uniform_stride,
                       sizeof(constants));

    Record(slot, slot_index);
    if (failed) {
        return;
    }
    recorded = true;
    want_layer = true;
}

void VrOverlayRenderer::Record(Slot& slot, u32 index) {
    const vk::CommandBuffer cmd = slot.cmd;
    if (cmd.reset() != vk::Result::eSuccess ||
        cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) !=
            vk::Result::eSuccess) {
        Disable("command buffer begin");
        return;
    }
    if (query_pool) {
        cmd.resetQueryPool(query_pool, index * 2, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, query_pool, index * 2);
    }
    const bool msaa = samples != vk::SampleCountFlagBits::e1;
    const vk::Image target = swapchain_images[image_index];
    std::array<vk::ImageMemoryBarrier2, 3> barriers{};
    u32 barrier_count = 0;
    // Contents are discarded: the whole image is cleared (or resolved) every frame.
    barriers[barrier_count++] = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = target,
        .subresourceRange = ColorRange,
    };
    if (msaa) {
        barriers[barrier_count++] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = msaa_color.image,
            .subresourceRange = ColorRange,
        };
    }
    barriers[barrier_count++] = {
        .srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                        vk::PipelineStageFlagBits2::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                        vk::PipelineStageFlagBits2::eLateFragmentTests,
        .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .image = depth.image,
        .subresourceRange =
            {
                .aspectMask = vk::ImageAspectFlagBits::eDepth,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    cmd.pipelineBarrier2({
        .imageMemoryBarrierCount = barrier_count,
        .pImageMemoryBarriers = barriers.data(),
    });

    const vk::RenderingAttachmentInfo color{
        .imageView = msaa ? msaa_color.view : swapchain_views[image_index],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .resolveMode = msaa ? vk::ResolveModeFlagBits::eAverage : vk::ResolveModeFlagBits::eNone,
        .resolveImageView = msaa ? swapchain_views[image_index] : vk::ImageView{},
        .resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = msaa ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.float32 = std::array{
                                                                      0.0f, 0.0f, 0.0f, 0.0f}}},
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth.view,
        .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eDontCare,
        .clearValue = vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = 0.0f,
                                                                                .stencil = 0}},
    };
    const u32 w = eye_extent.width, h = eye_extent.height;
    cmd.beginRendering({
        .renderArea = {.offset = {0, 0}, .extent = {2 * w, h}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth_attachment,
    });
    cmd.bindVertexBuffers(0, vertices.buffer, vk::DeviceSize{0});
    cmd.bindIndexBuffer(indices.buffer, 0, vk::IndexType::eUint32);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, slot.set, {});
    for (u32 eye = 0; eye < 2; ++eye) {
        cmd.setViewport(0, vk::Viewport{
                               .x = static_cast<float>(eye * w),
                               .y = 0.0f,
                               .width = static_cast<float>(w),
                               .height = static_cast<float>(h),
                               .minDepth = 0.0f,
                               .maxDepth = 1.0f,
                           });
        cmd.setScissor(0, vk::Rect2D{.offset = {static_cast<s32>(eye * w), 0}, .extent = {w, h}});
        vk::Pipeline bound{};
        for (const O::DrawItem& item : items) {
            const u32 mesh = static_cast<u32>(item.mesh);
            const vk::Pipeline pipeline = pipelines[PipelineIndex(item.pass)];
            if (mesh >= O::MeshCount || !pipeline || ranges[mesh].index_count == 0) {
                continue;
            }
            if (pipeline != bound) {
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
                bound = pipeline;
            }
            O::DrawConstants constants = O::MakeDrawConstants(item, eye);
            if (knee_active) {
                // The game layer compresses near disparity (DIBR): the item moves with this eye
                // by the part of its disparity the warp takes away, judged at its origin (the
                // vial, a controller and a hand are each a few centimetres deep).
                const O::Vec3 origin = item.model.Column(3);
                const float scale =
                    O::NearDisparityScale(O::Dot(origin - knee_centre, knee_forward),
                                          stereo_match.knee_m, stereo_match.min_m);
                const O::Vec3 shift =
                    knee_right * ((eye == 0 ? -1.0f : 1.0f) * (1.0f - scale) * knee_half_m);
                constants.model[12] += shift.x;
                constants.model[13] += shift.y;
                constants.model[14] += shift.z;
            }
            ScratchPush::Invalidate();
            cmd.pushConstants(pipeline_layout,
                              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              0, sizeof(constants), &constants);
            const O::MeshRange& range = ranges[mesh];
            cmd.drawIndexed(range.index_count, 1, range.first_index, range.vertex_offset, 0);
        }
    }
    cmd.endRendering();
    // The resolve (or the direct render) leaves the swapchain image in COLOR_ATTACHMENT_OPTIMAL,
    // the layout XR_KHR_vulkan_enable expects at release.
    if (query_pool) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, query_pool, index * 2 + 1);
    }
    if (cmd.end() != vk::Result::eSuccess) {
        Disable("command buffer end");
    }
}

const XrCompositionLayerBaseHeader* VrOverlayRenderer::SubmitLayer(const void* layer_next) {
    if (!initialized || failed) {
        return nullptr;
    }
    if (recorded) {
        recorded = false;
        Slot& slot = slots[slot_index];
        if (dev.device.resetFences(slot.fence) != vk::Result::eSuccess) {
            Disable("fence reset");
            return nullptr;
        }
        const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &slot.cmd};
        vk::Result submitted = vk::Result::eSuccess;
        {
            LockTrace::Phase phase{"overlay: vkQueueSubmit"}; // VR_LOCK_TRACE only
            submitted = dev.queue.submit(submit, slot.fence);
        }
        if (submitted != vk::Result::eSuccess) {
            VrDeviceLost::Note(submitted, "the VR overlay's submit");
            Disable(fmt::format("queue submit: {}", vk::to_string(submitted)));
            return nullptr;
        }
        slot.pending = true;
        // After the submit on the session's queue: the runtime sees the rendering complete.
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (const XrResult r = xrReleaseSwapchainImage(swapchain, &release); XR_FAILED(r)) {
            Disable(fmt::format("xrReleaseSwapchainImage failed ({})", static_cast<s32>(r)));
            return nullptr;
        }
        acquired = waited = false;
        shown = pending;
        shown_frame = frame;
        has_image = true;
        slot_index ^= 1;
        ++stats.drawn;
    }
    // Acquire the next image now, while the queue lock is held anyway; EndFrame waits for it.
    if (!acquired && !AcquireImage()) {
        return nullptr;
    }
    if (!want_layer || !has_image) {
        return nullptr;
    }
    want_layer = false;
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.next = layer_next;
    // Premultiplied alpha (no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT).
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = xr.space;
    layer.viewCount = 2;
    layer.views = layer_views.data();
    for (u32 i = 0; i < 2; ++i) {
        XrCompositionLayerProjectionView& view = layer_views[i];
        view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose = shown.pose[i];
        view.fov = shown.fov[i];
        view.subImage.swapchain = swapchain;
        view.subImage.imageRect = {{static_cast<s32>(i * eye_extent.width), 0},
                                   {static_cast<s32>(eye_extent.width),
                                    static_cast<s32>(eye_extent.height)}};
        view.subImage.imageArrayIndex = 0;
    }
    ++stats.layers;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
}

VrOverlayRenderer::Stats VrOverlayRenderer::TakeStats() {
    const Stats out = stats;
    stats = {};
    return out;
}

std::string VrOverlayRenderer::Describe() const {
    return fmt::format("{}x{} x2, {}, {}x MSAA, depth {}, {} MB", eye_extent.width,
                       eye_extent.height, vk::to_string(color_format), static_cast<u32>(samples),
                       vk::to_string(depth_format), memory_bytes / (1024 * 1024));
}

} // namespace Vulkan

#endif // ENABLE_OPENXR
