// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr_body_maps.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/vr_motion.h"
#include "video_core/host_shaders/vr_body_maps_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vram_report.h"

namespace Vulkan {
namespace {
struct Push {
    uint32_t triangles;
    uint32_t draws;
    uint32_t map_size;
    uint32_t tag;
    uint32_t table_at;
};

bool MakeBuffer(const Instance& instance, uint64_t bytes, bool host, vk::BufferUsageFlags usage,
                vk::Buffer& buffer, VmaAllocation& memory, void** mapped, uint64_t* address,
                const char* name) {
    VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = bytes,
                            .usage = static_cast<VkBufferUsageFlags>(usage)};
    if (address != nullptr) {
        info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (host) {
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VkBuffer raw{};
    VmaAllocationInfo details{};
    if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &raw, &memory, &details) !=
        VK_SUCCESS) {
        return false;
    }
    VideoCore::VramReport::Name(instance.GetAllocator(), memory,
                                fmt::format("vr body maps {} ({} KiB)", name, bytes >> 10));
    buffer = raw;
    if (mapped != nullptr) {
        *mapped = details.pMappedData;
        if (*mapped == nullptr) {
            return false;
        }
    }
    if (address != nullptr) {
        *address = instance.GetDevice().getBufferAddress({.buffer = buffer});
        if (*address == 0) {
            return false;
        }
    }
    return true;
}

void Free(const Instance& instance, vk::Buffer& buffer, VmaAllocation& memory) {
    if (buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer, memory);
        buffer = vk::Buffer{};
        memory = nullptr;
    }
}
} // namespace

VrBodyMaps::VrBodyMaps(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {
    draws.reserve(MaxDraws);
}

VrBodyMaps::~VrBodyMaps() {
    Free(instance, params, params_memory);
    Free(instance, positions, positions_memory);
    Free(instance, tables, tables_memory);
    Free(instance, pool, pool_memory);
    Free(instance, maps, maps_memory);
    Free(instance, dump, dump_memory);
}

bool VrBodyMaps::Init() {
    if (tried) {
        return ready.load(std::memory_order_relaxed);
    }
    tried = true;
    const auto features = instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics || !features.shaderInt64) {
        LOG_INFO(Render_Vulkan, "VR body maps: off (no vertex stores or 64-bit integers)");
        return false;
    }
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t b = 0; b < 4; ++b) {
        bindings[b] = {b, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute};
    }
    auto d = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = static_cast<uint32_t>(bindings.size()),
         .pBindings = bindings.data()});
    if (d.result != vk::Result::eSuccess) {
        return false;
    }
    descriptors = std::move(d.value);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                .pSetLayouts = &*descriptors,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push});
    if (l.result != vk::Result::eSuccess) {
        return false;
    }
    layout = std::move(l.value);
    const auto module =
        Compile(HostShaders::VR_BODY_MAPS_COMP, vk::ShaderStageFlagBits::eCompute, device);
    if (!module) {
        return false;
    }
    auto p = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
         .layout = *layout});
    device.destroyShaderModule(module);
    if (p.result != vk::Result::eSuccess) {
        return false;
    }
    pipeline = std::move(p.value);
    const auto storage = vk::BufferUsageFlagBits::eStorageBuffer;
    void* pool_data{};
    if (!MakeBuffer(instance, (1 + uint64_t(Slots) * MaxDraws) * sizeof(Shader::VrMotion::Params), true,
                    storage, params, params_memory, &params_mapped, &params_address,
                    "parameters") ||
        !MakeBuffer(instance, uint64_t(MaxVertices) * 16, false, storage, positions,
                    positions_memory, nullptr, &positions_address, "positions") ||
        !MakeBuffer(instance, uint64_t(Slots) * TableDwords * sizeof(uint32_t), true, storage,
                    tables, tables_memory, &tables_mapped, nullptr, "tables") ||
        !MakeBuffer(instance, PoolBytes, true, storage, pool, pool_memory, &pool_data, nullptr,
                    "index pool") ||
        !MakeBuffer(instance, MapsBytes(), false,
                    storage | vk::BufferUsageFlagBits::eTransferDst, maps, maps_memory, nullptr,
                    nullptr, "maps")) {
        LOG_ERROR(Render_Vulkan, "VR body maps: buffers could not be made; capsules only");
        return false;
    }
    pool_mapped = static_cast<uint8_t*>(pool_data);
    // Entry 0 stays zero: no store (Disabled).
    std::memset(params_mapped, 0,
                (1 + uint64_t(Slots) * MaxDraws) * sizeof(Shader::VrMotion::Params));
    ready.store(true, std::memory_order_release);
    LOG_INFO(Render_Vulkan,
             "VR body maps: ready ({} maps of {}x{}, {} positions, {} KiB index pool)", MaxMaps,
             MapSize, MapSize, MaxVertices, PoolBytes >> 10);
    return true;
}

uint32_t VrBodyMaps::PoolIndices(const uint8_t* data, uint32_t count, uint32_t index_size) {
    // The producer may ask before the recording thread made the buffers: it never makes them
    // (Init is the recording thread's); none until then.
    if (!Ready() || pool_mapped == nullptr || (index_size != 2 && index_size != 4)) {
        return ~0u;
    }
    const uint32_t bytes = count * index_size;
    // 4-byte aligned starts, in units of the mesh's own index size.
    const uint32_t at = (pool_used + 3u) & ~3u;
    if (count == 0 || bytes > PoolBytes || at > PoolBytes - bytes) {
        return ~0u;
    }
    std::memcpy(pool_mapped + at, data, bytes);
    pool_used = at + bytes;
    return at / index_size;
}

bool VrBodyMaps::AddDraw(uint64_t f, const Draw& draw, uint32_t& param, uint64_t& params_out,
                         uint64_t& positions_out) {
    if (!Init()) {
        return false;
    }
    if (f != frame) {
        frame = f;
        ++counter;
        tag = counter | 0x40000000u; // never 0, never a float of the positions' range alone
        missed = false;
        draws.clear();
        vertices = 0;
    }
    if (missed || draws.size() >= MaxDraws || draw.vertices == 0 ||
        draw.vertices > MaxVertices - vertices) {
        missed = true;
        return false;
    }
    const uint32_t slot = counter % Slots;
    const uint32_t index = 1 + slot * MaxDraws + static_cast<uint32_t>(draws.size());
    auto* entries = static_cast<Shader::VrMotion::Params*>(params_mapped);
    entries[index] = {.store_base = vertices,
                      .load_base = tag,
                      .vertices = draw.vertices,
                      .flags = Shader::VrMotion::FlagStore,
                      .first_vertex = draw.lowest + draw.vertex_offset,
                      .first_instance = draw.first_instance,
                      .instances = 1,
                      .reserved = 0};
    Draw stored = draw;
    stored.vertex_offset = vertices; // here: the first position
    draws.push_back(stored);
    vertices += draw.vertices;
    param = index;
    params_out = params_address;
    positions_out = positions_address;
    return true;
}

void VrBodyMaps::Missed(uint64_t f) {
    if (f != frame) {
        frame = f;
        ++counter;
        tag = counter | 0x40000000u;
        draws.clear();
        vertices = 0;
    }
    missed = true;
}

bool VrBodyMaps::Record(const RecorderProxy& cmd, uint64_t f, std::span<const Lamp> lamps) {
    built = {};
    built_frame = f;
    // (His draws were captured earlier in the frame, perhaps under a neighbouring camera record:
    // air_light.h FrameSlack. A frame without captures has `frame` reset: none.)
    if (!Ready() || !Shader::Optimization::AirLight::FrameNear(f, frame) || missed || draws.empty() ||
        lamps.empty()) {
        return false;
    }
    const uint32_t slot = counter % Slots;
    const uint32_t table_at = slot * TableDwords;
    auto* table = static_cast<uint32_t*>(tables_mapped) + table_at;
    std::memset(table, 0, TableDwords * sizeof(uint32_t));
    const auto put = [&](uint32_t at, float v) { std::memcpy(&table[at], &v, sizeof(v)); };
    // The maps: clip -> world (his draws share the cascade's camera: the first's) -> the lamp.
    const auto& c = draws.front().clip_to_world;
    uint32_t used = 0;
    for (uint32_t k = 0; k < MaxMaps; ++k) {
        const uint32_t m = k * TableMapDwords;
        table[m + 14] = 0xFFFFFFFFu;
        if (k >= lamps.size()) {
            continue;
        }
        const Lamp& lamp = lamps[k];
        // q = A (C x + c - origin), A the lamp's axes.
        for (uint32_t r = 0; r < 3; ++r) {
            double row[4]{};
            for (uint32_t j = 0; j < 3; ++j) {
                for (uint32_t i = 0; i < 4; ++i) {
                    row[i] += lamp.axes[r * 3 + j] * c[j * 4 + i];
                }
                row[3] -= lamp.axes[r * 3 + j] * lamp.origin[j];
            }
            for (uint32_t i = 0; i < 4; ++i) {
                put(m + r * 4 + i, float(row[i]));
            }
        }
        put(m + 12, lamp.f);
        put(m + 13, lamp.near_z);
        table[m + 14] = k * MapSize * MapSize;
        built[k] = {.valid = true, .lamp = lamp, .base = k * MapSize * MapSize};
        ++used;
    }
    uint32_t triangles = 0;
    for (uint32_t i = 0; i < draws.size(); ++i) {
        const Draw& d = draws[i];
        const uint32_t at = MaxMaps * TableMapDwords + i * TableDrawDwords;
        table[at + 0] = triangles;
        table[at + 1] = d.count / 3;
        table[at + 2] = d.pool_first;
        table[at + 3] = d.short_indices ? 1u : 0u;
        table[at + 4] = d.vertex_offset; // its first position (AddDraw)
        table[at + 5] = d.lowest;
        table[at + 6] = d.vertices;
        triangles += d.count / 3;
    }
    // Earlier reads of the maps (the last frame's lamp draws) before they are cleared; the
    // positions' stores (this frame's shadow pass) before they are read.
    const std::array<vk::BufferMemoryBarrier2, 2> before{{
        {.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader |
                         vk::PipelineStageFlagBits2::eComputeShader,
         .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = maps,
         .offset = 0,
         .size = VK_WHOLE_SIZE},
        {.srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
         .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = positions,
         .offset = 0,
         .size = VK_WHOLE_SIZE},
    }};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 2,
                                            .pBufferMemoryBarriers = before.data()});
    cmd.fillBuffer(maps, 0, MapsBytes(), 0x7F7FFFFFu); // the largest float: nothing of him
    const vk::BufferMemoryBarrier2 cleared{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = maps,
        .offset = 0,
        .size = VK_WHOLE_SIZE};
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &cleared});
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const std::array<vk::DescriptorBufferInfo, 4> infos{{
        {positions, 0, VK_WHOLE_SIZE},
        {pool, 0, VK_WHOLE_SIZE},
        {tables, 0, VK_WHOLE_SIZE},
        {maps, 0, VK_WHOLE_SIZE},
    }};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    for (uint32_t b = 0; b < 4; ++b) {
        writes[b] = {.dstBinding = b,
                     .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                     .pBufferInfo = &infos[b]};
    }
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    const Push push{.triangles = triangles,
                    .draws = static_cast<uint32_t>(draws.size()),
                    .map_size = MapSize,
                    .tag = tag,
                    .table_at = table_at};
    cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmd.dispatch((triangles + 63) / 64, MaxMaps, 1);
    // The maps before the lamp draws read them; this frame's reads of the positions before the
    // next frame's shadow pass stores them again.
    const std::array<vk::BufferMemoryBarrier2, 2> after{{
        {.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
         .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = maps,
         .offset = 0,
         .size = VK_WHOLE_SIZE},
        {.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
         .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
         .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = positions,
         .offset = 0,
         .size = VK_WHOLE_SIZE},
    }};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 2,
                                            .pBufferMemoryBarriers = after.data()});
    static const bool dump_maps = [] {
        const char* const env = std::getenv("VR_BODY_MAPS_DUMP");
        return env != nullptr && env[0] == '1';
    }();
    if (dump_maps && built_count++ % 300 == 150) {
        if (!dump && !MakeBuffer(instance, MapsBytes(), true,
                                 vk::BufferUsageFlagBits::eTransferDst |
                                     vk::BufferUsageFlagBits::eStorageBuffer,
                                 dump, dump_memory, &dump_mapped, nullptr, "dump")) {
            dump = vk::Buffer{};
        }
        if (dump) {
            const vk::BufferMemoryBarrier2 to_copy{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = maps,
                .offset = 0,
                .size = VK_WHOLE_SIZE};
            cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                                    .pBufferMemoryBarriers = &to_copy});
            cmd.copyBuffer(maps, dump, vk::BufferCopy{0, 0, MapsBytes()});
            const uint32_t n = built_count;
            const void* src = dump_mapped;
            LOG_INFO(Render_Vulkan,
                     "VR body maps: dump {}: {} draws, {} positions, {} triangles, {} map(s), lamp "
                     "0 at ({:.2f} {:.2f} {:.2f}) f {:.3f} near {:.3f}",
                     n, draws.size(), vertices, triangles, used, built[0].lamp.origin[0],
                     built[0].lamp.origin[1], built[0].lamp.origin[2], built[0].lamp.f,
                     built[0].lamp.near_z);
            scheduler.DeferOperation([src, n] {
                std::ofstream out(fmt::format("user/log/body_maps_{}.bin", n), std::ios::binary);
                out.write(static_cast<const char*>(src), std::streamsize(MapsBytes()));
                LOG_INFO(Render_Vulkan, "VR body maps: dumped user/log/body_maps_{}.bin", n);
            });
        }
    }
    // The frame's captures are spent: a second lamp pass (a reflection's) finds the maps built.
    draws.clear();
    frame = ~0ull;
    return used != 0;
}

std::array<VrBodyMaps::Built, VrBodyMaps::MaxMaps> VrBodyMaps::Maps(uint64_t f) const {
    // (Built at the frame's first lamp draw; a later lamp draw's record may be its neighbour.)
    return Shader::Optimization::AirLight::FrameNear(f, built_frame) ? built : std::array<Built, MaxMaps>{};
}

} // namespace Vulkan
