// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_fog_grid.h"
#include <array>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "video_core/host_shaders/vr_fog_grid_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vram_report.h"

namespace Vulkan {

namespace Grid = Shader::Optimization::FogGrid;

VrFogGrid::VrFogGrid(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}

VrFogGrid::~VrFogGrid() {
    if (buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer, memory);
    }
    const auto device = instance.GetDevice();
    for (Picture* picture : {&lit[0], &lit[1], &no_atlas, &world, &stir[0], &stir[1]}) {
        if (picture->view) {
            device.destroyImageView(picture->view);
        }
        if (picture->image) {
            vmaDestroyImage(instance.GetAllocator(), picture->image, picture->memory);
        }
    }
}

bool VrFogGrid::Init() {
    if (tried) {
        return ready;
    }
    tried = true;
    const auto device = instance.GetDevice();
    const auto physical = instance.GetPhysicalDevice();
    const auto stage = vk::ShaderStageFlagBits::eCompute;
    // The lit shares' format: one 16-bit float a cell where the device can write and blend it,
    // else four (every device can).
    const auto usable = [&](vk::Format format) {
        const auto features = physical.getFormatProperties(format).optimalTilingFeatures;
        return (features & vk::FormatFeatureFlagBits::eStorageImage) &&
               (features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
    };
    const bool one = physical.getFeatures().shaderStorageImageExtendedFormats && usable(vk::Format::eR16Sfloat);
    lit_format = one ? vk::Format::eR16Sfloat : vk::Format::eR16G16B16A16Sfloat;
    const auto make_sampler = [&](vk::Filter filter) {
        return device.createSamplerUnique({.magFilter = filter,
                                           .minFilter = filter,
                                           .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                           .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                           .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                           .addressModeW = vk::SamplerAddressMode::eClampToEdge});
    };
    auto n = make_sampler(vk::Filter::eNearest);
    auto l = make_sampler(vk::Filter::eLinear);
    if (n.result != vk::Result::eSuccess || l.result != vk::Result::eSuccess) {
        return false;
    }
    nearest = std::move(n.value);
    linear = std::move(l.value);
    const vk::Sampler atlas_sampler = *nearest, lit_sampler = *linear;
    // The parameters, the buffer it writes, the moon's shadow atlas, the last build's lit shares,
    // this build's.
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, stage},
        {1, vk::DescriptorType::eStorageBuffer, 1, stage},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, stage, &atlas_sampler},
        {3, vk::DescriptorType::eCombinedImageSampler, 1, stage, &lit_sampler},
        {4, vk::DescriptorType::eStorageImage, 1, stage},
        {5, vk::DescriptorType::eCombinedImageSampler, 1, stage, &lit_sampler}, // the world-fixed volume
    }};
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = static_cast<uint32_t>(bindings.size()),
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess) {
        return false;
    }
    descriptors = std::move(d.value);
    auto pl = device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*descriptors});
    if (pl.result != vk::Result::eSuccess) {
        return false;
    }
    layout = std::move(pl.value);
    // The world-fixed volume: two 16-bit floats a cube where the device can write and blend
    // them, else four.
    const bool two = physical.getFeatures().shaderStorageImageExtendedFormats && usable(vk::Format::eR16G16Sfloat);
    world_format = two ? vk::Format::eR16G16Sfloat : vk::Format::eR16G16B16A16Sfloat;
    const char* const lit_define = one ? "LIT_FORMAT=r16f" : "LIT_FORMAT=rgba16f";
    const char* const world_define = two ? "WORLD_FORMAT=rg16f" : "WORLD_FORMAT=rgba16f";
    {
        const std::array<vk::DescriptorSetLayoutBinding, 3> world_bindings{{
            {0, vk::DescriptorType::eStorageBuffer, 1, stage},
            {5, vk::DescriptorType::eStorageImage, 1, stage},
            {6, vk::DescriptorType::eCombinedImageSampler, 1, stage, &lit_sampler}, // the stir field
        }};
        auto wd = device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = static_cast<uint32_t>(world_bindings.size()),
             .pBindings = world_bindings.data()});
        if (wd.result != vk::Result::eSuccess) {
            return false;
        }
        world_descriptors = std::move(wd.value);
        auto wl = device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*world_descriptors});
        if (wl.result != vk::Result::eSuccess) {
            return false;
        }
        world_layout = std::move(wl.value);
        const auto world_module =
            Compile(HostShaders::VR_FOG_GRID_COMP, stage, device, {lit_define, world_define, "WORLD_VOLUME=1"});
        if (!world_module) {
            return false;
        }
        auto wp = device.createComputePipelineUnique(
            {}, {.stage = {.stage = stage, .module = world_module, .pName = "main"}, .layout = *world_layout});
        device.destroyShaderModule(world_module);
        if (wp.result != vk::Result::eSuccess) {
            return false;
        }
        world_pipeline = std::move(wp.value);
    }
    // The stir field's pass (fog_grid.h Stir): the parameters, the field it writes, the last one.
    {
        const std::array<vk::DescriptorSetLayoutBinding, 3> stir_bindings{{
            {0, vk::DescriptorType::eStorageBuffer, 1, stage},
            {6, vk::DescriptorType::eStorageImage, 1, stage},
            {7, vk::DescriptorType::eCombinedImageSampler, 1, stage, &lit_sampler},
        }};
        auto sd = device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = static_cast<uint32_t>(stir_bindings.size()),
             .pBindings = stir_bindings.data()});
        if (sd.result != vk::Result::eSuccess) {
            return false;
        }
        stir_descriptors = std::move(sd.value);
        auto sl = device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*stir_descriptors});
        if (sl.result != vk::Result::eSuccess) {
            return false;
        }
        stir_layout = std::move(sl.value);
        const auto stir_module =
            Compile(HostShaders::VR_FOG_GRID_COMP, stage, device, {lit_define, world_define, "STIR_FIELD=1"});
        if (!stir_module) {
            return false;
        }
        auto sp = device.createComputePipelineUnique(
            {}, {.stage = {.stage = stage, .module = stir_module, .pName = "main"}, .layout = *stir_layout});
        device.destroyShaderModule(stir_module);
        if (sp.result != vk::Result::eSuccess) {
            return false;
        }
        stir_pipeline = std::move(sp.value);
    }
    const auto module = Compile(HostShaders::VR_FOG_GRID_COMP, stage, device, {lit_define, world_define});
    if (!module) {
        return false;
    }
    auto p = device.createComputePipelineUnique(
        {}, {.stage = {.stage = stage, .module = module, .pName = "main"}, .layout = *layout});
    device.destroyShaderModule(module);
    if (p.result != vk::Result::eSuccess) {
        return false;
    }
    pipeline = std::move(p.value);
    // One texel for the atlas' binding while the game has none (the parameters say "no shadows":
    // it is never read).
    if (!Make(no_atlas, 1, 1, 0, "vr fog grid (no atlas)")) {
        return false;
    }
    // The world-fixed volume's picture: x and z across, y its depth (fog_grid.h World).
    if (!Make(world, Grid::World::SizeX, Grid::World::SizeZ, Grid::World::SizeY, "vr fog grid (world volume)",
              world_format)) {
        return false;
    }
    // The stir field's two pictures: a column of the volume a texel (fog_grid.h Stir).
    for (Picture& picture : stir) {
        if (!Make(picture, Grid::World::SizeX, Grid::World::SizeZ, 0, "vr fog grid (stir field)",
                  vk::Format::eR16G16B16A16Sfloat)) {
            return false;
        }
    }
    ready = true;
    LOG_INFO(Render_Vulkan, "VR fog grid: ready (lit shares as {})", one ? "r16f" : "rgba16f");
    return true;
}

// A picture of the lit shares (z cells deep), or a flat one (z 0).
bool VrFogGrid::Make(Picture& picture, uint32_t x, uint32_t y, uint32_t z, const char* name, vk::Format format) {
    if (format == vk::Format{}) {
        format = lit_format;
    }
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = z ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {x, y, z ? z : 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc, &raw, &picture.memory, nullptr) != VK_SUCCESS) {
        picture.memory = nullptr;
        LOG_ERROR(Render_Vulkan, "VR fog grid: no picture ({}x{}x{})", x, y, z);
        return false;
    }
    VideoCore::VramReport::Name(instance.GetAllocator(), picture.memory, name);
    picture.image = raw;
    auto view = instance.GetDevice().createImageView(
        {.image = picture.image,
         .viewType = z ? vk::ImageViewType::e3D : vk::ImageViewType::e2D,
         .format = format,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    if (view.result != vk::Result::eSuccess) {
        vmaDestroyImage(instance.GetAllocator(), picture.image, picture.memory);
        picture = {};
        return false;
    }
    picture.view = view.value;
    picture.fresh = true;
    return true;
}

// Freed once the GPU is past the frames that use it.
void VrFogGrid::Free(Picture& picture) {
    if (picture.image) {
        scheduler.DeferOperation(
            [device = instance.GetDevice(), allocator = instance.GetAllocator(), old = picture] {
                device.destroyImageView(old.view);
                vmaDestroyImage(allocator, old.image, old.memory);
            });
    }
    picture = {};
}

bool VrFogGrid::Size(uint32_t x, uint32_t y, uint32_t z) {
    if (x == size_x && y == size_y && z == size_z && buffer && lit[0].image && lit[1].image) {
        return true;
    }
    if (buffer) {
        // Another size: the old buffer freed once the GPU is past the frames that use it.
        scheduler.DeferOperation([allocator = instance.GetAllocator(), old = buffer, old_memory = memory] {
            vmaDestroyBuffer(allocator, old, old_memory);
        });
        buffer = vk::Buffer{};
        memory = nullptr;
        bytes = 0;
    }
    Free(lit[0]);
    Free(lit[1]);
    lit_written = false;
    recorded = false;
    size_x = size_y = size_z = 0;
    const uint64_t size = Grid::BufferDwords(x, y, z) * sizeof(uint32_t);
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = size,
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer raw{};
    if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &raw, &memory, nullptr) != VK_SUCCESS) {
        memory = nullptr;
        LOG_ERROR(Render_Vulkan, "VR fog grid: no buffer ({} KiB)", size >> 10);
        return false;
    }
    VideoCore::VramReport::Name(instance.GetAllocator(), memory, fmt::format("vr fog grid ({} KiB)", size >> 10));
    buffer = raw;
    bytes = size;
    if (!Make(lit[0], x, y, z, "vr fog grid (lit shares)") || !Make(lit[1], x, y, z, "vr fog grid (lit shares)")) {
        return false;
    }
    size_x = x;
    size_y = y;
    size_z = z;
    LOG_INFO(Render_Vulkan, "VR fog grid: {}x{}x{} cells, {} KiB, two pictures of the lit shares", x, y, z,
             size >> 10);
    return true;
}

bool VrFogGrid::Prepare(uint32_t x, uint32_t y, uint32_t z, bool& past) {
    past = false;
    if (x == 0 || y == 0 || z == 0 || x > 512 || y > 512 || z > 256 || !Init() || !Size(x, y, z)) {
        return false;
    }
    past = lit_written;
    return true;
}

void VrFogGrid::Record(const RecorderProxy& cmd, const vk::DescriptorBufferInfo& params, vk::ImageView atlas,
                       vk::ImageLayout atlas_layout, bool shadows, bool volume, bool stir_on) {
    // Pictures just made: into the layout they keep.
    std::array<vk::ImageMemoryBarrier2, 6> made;
    uint32_t made_count = 0;
    for (Picture* picture : {&lit[0], &lit[1], &no_atlas, &world, &stir[0], &stir[1]}) {
        if (picture->fresh) {
            picture->fresh = false;
            made[made_count++] = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eNone,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = picture->image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        }
    }
    // What came before: the last scene copy's reads of the buffer, our own last dispatch (the
    // buffer and the lit shares it wrote).
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                            .pMemoryBarriers = &before,
                                            .imageMemoryBarrierCount = made_count,
                                            .pImageMemoryBarriers = made.data()});
    const vk::DescriptorImageInfo world_image{vk::Sampler{}, world.view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo stir_image{vk::Sampler{}, stir[stir_now].view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo stir_before{vk::Sampler{}, stir[stir_now ^ 1].view, vk::ImageLayout::eGeneral};
    const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                     .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                     .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
    if (volume && stir_on) {
        // The stir field's frame first (a thread a column): from the last frame's field to this one's.
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *stir_pipeline);
        const std::array<vk::WriteDescriptorSet, 3> stir_writes{{
            {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &params},
            {.dstBinding = 6, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &stir_image},
            {.dstBinding = 7, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &stir_before},
        }};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *stir_layout, 0, stir_writes);
        cmd.dispatch((Grid::World::SizeX + 7) / 8, (Grid::World::SizeZ + 7) / 8, 1);
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &written});
    }
    if (volume) {
        // The world-fixed volume first (a thread a column of cubes), then the grid reads it.
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *world_pipeline);
        const std::array<vk::WriteDescriptorSet, 3> world_writes{{
            {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &params},
            {.dstBinding = 5, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &world_image},
            {.dstBinding = 6, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &stir_image},
        }};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *world_layout, 0, world_writes);
        cmd.dispatch((Grid::World::SizeX + 7) / 8, (Grid::World::SizeZ + 7) / 8, 1);
        const vk::MemoryBarrier2 built{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                       .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                       .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                       .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &built});
    }
    if (volume && stir_on) {
        stir_now ^= 1; // (this frame's field is the next one's "last")
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const vk::DescriptorBufferInfo out{buffer, 0, bytes};
    const std::array<vk::DescriptorImageInfo, 3> images{{
        {vk::Sampler{}, atlas ? atlas : no_atlas.view, atlas ? atlas_layout : vk::ImageLayout::eGeneral},
        {vk::Sampler{}, lit[now ^ 1].view, vk::ImageLayout::eGeneral},
        {vk::Sampler{}, lit[now].view, vk::ImageLayout::eGeneral},
    }};
    const std::array<vk::WriteDescriptorSet, 6> writes{{
        {.dstBinding = 5, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &world_image},
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &params},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &out},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[0]},
        {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &images[1]},
        {.dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &images[2]},
    }};
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    cmd.dispatch((size_x + 7) / 8, (size_y + 7) / 8, 1); // a thread a column
    // The buffer before the scene's copy reads it.
    const vk::BufferMemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                         .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                                         .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .buffer = buffer,
                                         .offset = 0,
                                         .size = VK_WHOLE_SIZE};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &after});
    // This build's lit shares are the next one's past (a build without shadows wrote none).
    lit_written = shadows;
    recorded = true;
    if (shadows) {
        now ^= 1;
    }
}

} // namespace Vulkan
