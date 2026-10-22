// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Adapted from bbport ObjectMotion; native stereo, lazy activation, nonblocking retirement.
#include <algorithm>
#include <cstring>
#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/renderer_vulkan/vr_temporal_optimizations.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_object_motion.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
#include <vk_mem_alloc.h>
namespace Vulkan {
namespace {
// Bit 37: value-only barriers go through the recorder (bbport records its motion pass through
// the scheduler). Off: the raw borrow, which drains the recorder and keeps the remaining
// draws of this command buffer on the calling thread.
template <class Raw>
void RecordBarrier(Scheduler& scheduler, const vk::DependencyInfo& info, Raw&& raw) {
    if (UpscalerFastOn(UpscalerFastPart::Recording)) {
        scheduler.Commands().pipelineBarrier2(info);
        UpscalerFastNote(g_upscaler_fast.raw_avoided);
    } else {
        raw().pipelineBarrier2(info);
    }
}
bool Buffer(const Instance& instance, uint64_t bytes, bool host, vk::Buffer& buffer,
            VmaAllocation& allocation, void** mapped, uint64_t& address) {
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = bytes,
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo memory{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (host) {
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VkBuffer raw{};
    VmaAllocationInfo details{};
    if (vmaCreateBuffer(instance.GetAllocator(), &info, &memory, &raw, &allocation, &details) !=
        VK_SUCCESS)
        return false;
    buffer = raw;
    if (mapped)
        *mapped = details.pMappedData;
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address && (!host || details.pMappedData);
}
constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2};
} // namespace
VrObjectMotion::VrObjectMotion(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
VrObjectMotion::~VrObjectMotion() {
    Release();
}
void VrObjectMotion::Release() {
    for (auto& target : targets)
        Retire(target);
    if (parameters || positions) {
        // Command-buffer retirement also covers shader BDA reads and writes. No unconditional
        // Finish: OFF instances have no resources, and live resources leave with their ticket.
        scheduler.DeferOperation([allocator = instance.GetAllocator(), p = parameters,
                                  q = positions, pa = parameters_allocation,
                                  qa = positions_allocation] {
            if (p)
                vmaDestroyBuffer(allocator, p, pa);
            if (q)
                vmaDestroyBuffer(allocator, q, qa);
        });
    }
    parameters = nullptr;
    positions = nullptr;
    parameters_allocation = nullptr;
    positions_allocation = nullptr;
    mapped = nullptr;
    specialization = {};
    history.reset();
    active = false;
}
bool VrObjectMotion::Init() {
    const auto features = instance.GetPhysicalDevice().getFeatures();
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    if (!features.vertexPipelineStoresAndAtomics || !features.shaderInt64 ||
        limits.maxColorAttachments < 8 || limits.maxVertexOutputComponents < 128 ||
        limits.maxFragmentInputComponents < 128)
        return false;
    constexpr auto format = vk::Format::eR32G32B32A32Sfloat;
    const auto support =
        instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    const auto required =
        vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage;
    if ((support & required) != required)
        return false;
    if (!position_capacity) position_capacity = TemporalOptimizations() ? 4u << 20 : PositionsPerEye;
    history = std::make_unique<VrObjectMotionState>(position_capacity);
    LOG_INFO(Render_Vulkan,"VR temporal history: capacity={} positions/eye, bytes={}", position_capacity,history->PositionBytes());
    void* data{};
    if (!Buffer(instance,
                uint64_t(VrObjectMotionState::ParameterCount) * sizeof(Shader::VrMotion::Params),
                true, parameters, parameters_allocation, &data, specialization.params_address) ||
        !Buffer(instance, history->PositionBytes(), false, positions, positions_allocation, nullptr,
                specialization.positions_address))
        return false;
    mapped = static_cast<Shader::VrMotion::Params*>(data);
    mapped[0] = {};
    mapped[1] = {}; // Inactive pair must still be readable by either eye.
    specialization.enabled = true;
    return true;
}
bool VrObjectMotion::Begin(const VR::Graphics::Config& accepted, bool provider_ready, uint64_t id,
                           uint64_t epoch, bool camera_cut) {
    active = false;
    if (accepted.upscaler == VR::Graphics::Upscaler::Off || !provider_ready) {
        if (parameters || positions)
            Release();
        return false;
    }
    if (failed)
        return false;
    if (grow_pending && position_capacity < PositionsPerEye) {
        Release();
        position_capacity = std::min(position_capacity * 2, PositionsPerEye);
        grow_pending = false;
        camera_cut = true;
    }
    if (!specialization.enabled && !Init()) {
        failed = true;
        Release();
        LOG_ERROR(Render_Vulkan, "VR temporal object motion: resource initialization failed");
        return false;
    }
    if (id % 600 == 0) LOG_INFO(Render_Vulkan,"VR temporal history: used={} capacity={} positions/eye",history->Used(),position_capacity);
    const auto ticket = history->ReuseTicket(id);
    if (ticket && !scheduler.IsFree(ticket))
        return false;
    if (!history->Begin(id, epoch, scheduler.GetMasterSemaphore()->KnownGpuTick(), true,
                        camera_cut))
        return false;
    frame = id;
    active = true;
    for (auto& target : targets)
        target.written = false;
    scheduler.EndRendering();
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = positions,
        .offset = 0,
        .size = VK_WHOLE_SIZE};
    RecordBarrier(scheduler, {.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier},
            [&] { return scheduler.CommandBuffer(); });
    return true;
}
Shader::VrMotion VrObjectMotion::Specialization() const {
    return active ? specialization : Shader::VrMotion{};
}
uint32_t VrObjectMotion::Prepare(const Motion::Draw& draw) {
    ScratchProfile::Cpu scratch_motion(44, "VrObjectMotion::Prepare");
    if (!active)
        return 0;
    const auto exhausted = history->Stats().exhausted;
    const auto p = history->Prepare(draw, scheduler.CurrentTick());
    if (history->Stats().exhausted != exhausted) {
        grow_pending = true;
        return 0; // Refuse this incomplete frame; rebuild a larger history next frame.
    }
    if (p.index)
        std::copy(p.eyes.begin(), p.eyes.end(), mapped + p.index);
    else if (history->Stats().invalid + history->Stats().exhausted <= 3)
        LOG_WARNING(Render_Vulkan, "VR temporal motion allocation refused: vertices={} instances={} exhausted={} invalid={}",
            draw.vertices.count,draw.instances,history->Stats().exhausted,history->Stats().invalid);
    return p.index;
}
void VrObjectMotion::Retire(Target& t) {
    if (!t.image)
        return;
    scheduler.DeferOperation([image = std::move(t.image), array = std::move(t.array_view),
                              eyes = std::move(t.eye_views)]() mutable {
        for (auto& eye : eyes)
            eye.reset();
        array.reset();
        image.Destroy();
    });
    t.width = t.height = 0;
    t.used_frame = 0;
    t.written = false;
    t.layout = vk::ImageLayout::eUndefined;
}
VrObjectMotion::Target* VrObjectMotion::GetTarget(uint32_t w, uint32_t h) {
    for (auto& t : targets)
        if (t.image && t.width == w && t.height == h) {
            t.used_frame = frame;
            return &t;
        }
    Target* slot{};
    for (auto& t : targets) {
        if (!t.image) {
            slot = &t;
            break;
        }
        // Never evict vectors from another scene pass in this frame.
        if (t.used_frame != frame && (!slot || t.used_frame < slot->used_frame))
            slot = &t;
    }
    if (!slot)
        return nullptr;
    Retire(*slot);
    const auto device = instance.GetDevice();
    slot->image = VideoCore::UniqueImage(device, instance.GetAllocator());
    slot->image.Create(
        {.imageType = vk::ImageType::e2D,
         .format = vk::Format::eR32G32B32A32Sfloat,
         .extent = {w, h, 1},
         .mipLevels = 1,
         .arrayLayers = 2,
         .samples = vk::SampleCountFlagBits::e1,
         .tiling = vk::ImageTiling::eOptimal,
         .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
         .initialLayout = vk::ImageLayout::eUndefined});
    auto view = device.createImageViewUnique({.image = slot->image,
                                              .viewType = vk::ImageViewType::e2DArray,
                                              .format = vk::Format::eR32G32B32A32Sfloat,
                                              .subresourceRange = Range});
    if (view.result != vk::Result::eSuccess) {
        Retire(*slot);
        return nullptr;
    }
    slot->array_view = std::move(view.value);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto v = device.createImageViewUnique(
            {.image = slot->image,
             .viewType = vk::ImageViewType::e2D,
             .format = vk::Format::eR32G32B32A32Sfloat,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, eye, 1}});
        if (v.result != vk::Result::eSuccess) {
            Retire(*slot);
            return nullptr;
        }
        slot->eye_views[eye] = std::move(v.value);
    }
    slot->width = w;
    slot->height = h;
    slot->used_frame = frame;
    return slot;
}
bool VrObjectMotion::Attach(RenderState& state) {
    if (!active || state.probe_view_mask != 3 || !state.width || !state.height ||
        state.num_color_attachments > Shader::VrMotion::Output ||
        state.color_attachments[Shader::VrMotion::Output].image_view)
        return false;
    auto* t = GetTarget(state.width, state.height);
    if (!t)
        return false;
    auto& attachment = state.color_attachments[Shader::VrMotion::Output];
    attachment = {};
    attachment.image_view = *t->array_view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!t->written || t->layout != attachment.image_layout) {
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = t->layout,
            .newLayout = attachment.image_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t->image,
            .subresourceRange = Range};
        RecordBarrier(scheduler, {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                [&] { return scheduler.CommandBuffer(); });
        attachment.is_clear = !t->written;
        t->written = true;
        t->layout = attachment.image_layout;
    }
    state.num_color_attachments = Shader::VrMotion::Output + 1;
    return true;
}
std::array<vk::ImageView, 2> VrObjectMotion::PrepareRead(uint32_t w, uint32_t h) {
    if (!active)
        return {};
    for (auto& t : targets)
        if (t.image && t.width == w && t.height == h && t.written) {
            scheduler.EndRendering();
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = t.layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = t.image,
                .subresourceRange = Range};
            RecordBarrier(scheduler, {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                    [&] { return scheduler.CommandBuffer(); });
            t.layout = vk::ImageLayout::eGeneral;
            return {*t.eye_views[0], *t.eye_views[1]};
        }
    return {};
}
} // namespace Vulkan
