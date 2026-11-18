// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocation_T;

namespace Vulkan {
class Instance;
class Scheduler;
class RecorderProxy;

// The fog grid (shader_recompiler/ir/passes/fog_grid.h): one box of air cells for both eyes, built
// once a frame by a dispatch of our own before the scene's copy draws. This is the GPU side: the
// pipeline (host_shaders/vr_fog_grid.comp: each column's mist and moonlit air, the moon's shadow
// in every cell, summed front to back), the buffer it writes and the scene's copy reads, and two
// small pictures of the cells' lit shares (this build's and the last one's: the history). Nothing
// is made until the first Prepare (the grid is off unless VR_FOG_GRID asks for it).
class VrFogGrid {
public:
    VrFogGrid(const Instance& instance, Scheduler& scheduler);
    ~VrFogGrid();

    // Recording thread: the pipeline, the buffer and the pictures for x * y * z cells. False when
    // they cannot be made. `past`: the last build's lit shares are there to be read (the same
    // size, written by the build before this one).
    bool Prepare(uint32_t x, uint32_t y, uint32_t z, bool& past);
    // Recording thread, outside a render pass, after Prepare: this frame's grid from `params`
    // (fog_grid.h's parameters, a storage buffer range that holds the same size). `atlas`: the
    // moon's shadow atlas as it is now (null: none; the parameters then say "no shadows").
    // `shadows`: this build writes the lit shares (the next one may read them).
    // `volume`: the world-fixed volume is built first (fog_grid.h World) and the grid reads it.
    // `stir`: the stir field's frame before that (fog_grid.h Stir), read by the volume's build.
    void Record(const RecorderProxy& cmd, const vk::DescriptorBufferInfo& params, vk::ImageView atlas,
                vk::ImageLayout atlas_layout, bool shadows, bool volume, bool stir);
    // The buffer the scene's copy binds and its size (null until the first Prepare).
    // The buffer holds a grid: a dispatch has been recorded into it since it was made (until
    // then its memory is whatever the allocation held).
    bool Built() const {
        return recorded;
    }
    vk::Buffer Buffer() const {
        return buffer;
    }
    uint64_t Bytes() const {
        return bytes;
    }

private:
    struct Picture {
        vk::Image image{};
        VmaAllocation_T* memory{};
        vk::ImageView view{};
        bool fresh{}; // just made: its layout is still undefined
    };
    bool Init();
    bool Size(uint32_t x, uint32_t y, uint32_t z);
    bool Make(Picture& picture, uint32_t x, uint32_t y, uint32_t z, const char* name, vk::Format format = {});
    void Free(Picture& picture);

    const Instance& instance;
    Scheduler& scheduler;
    bool tried{}, ready{};
    vk::Format lit_format{};
    vk::UniqueSampler nearest, linear;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::Buffer buffer{};
    VmaAllocation_T* memory{};
    uint64_t bytes{};
    uint32_t size_x{}, size_y{}, size_z{};
    // The lit shares: lit[now] is written by the next build, lit[now ^ 1] holds the last build's
    // (when `lit_written`). `no_atlas`: one texel bound in the atlas' place while there is none.
    Picture lit[2]{}, no_atlas{};
    // The world-fixed volume (fog_grid.h World): its picture, its own pipeline (the same shader
    // built with WORLD_VOLUME) and layout (the parameters and the picture it writes).
    Picture world{};
    vk::Format world_format{};
    vk::UniqueDescriptorSetLayout world_descriptors;
    vk::UniquePipelineLayout world_layout;
    vk::UniquePipeline world_pipeline;
    // The stir field (fog_grid.h Stir): stir[stir_now] is written by the next frame's pass from
    // stir[stir_now ^ 1], the last one's; its pipeline (the same shader built with STIR_FIELD).
    Picture stir[2]{};
    uint32_t stir_now{};
    vk::UniqueDescriptorSetLayout stir_descriptors;
    vk::UniquePipelineLayout stir_layout;
    vk::UniquePipeline stir_pipeline;
    uint32_t now{};
    bool lit_written{};
    bool recorded{}; // a dispatch has been recorded into this buffer (Built)
};

} // namespace Vulkan
