// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocation_T;

namespace Vulkan {
class Instance;
class Scheduler;
class RecorderProxy;

// The nearest-depth pyramid for the lamp shadows' march (2026-11-13, the user: "improve visuals AND
// performance with it"; light_shadow.h SceneMaps::Pyramid). Before the frame's AO pass, one
// dispatch (host_shaders/vr_depth_pyramid.comp) takes the depth that pass reads, both eyes, to
// blocks of 2 to 32 texels a side, each block's nearest and farthest raw depth. The AO pass's
// march checks each step's whole stretch on the picture against the blocks covering it: in front
// of everything there, nothing is read; else the stretch is read finely, so a thin blocker between
// two steps is never stepped over and the steps can be fewer. One small buffer (~1.4 MB for both
// eyes at 668 x 720), one dispatch a frame, nothing waits.
class VrDepthPyramid {
public:
    static constexpr uint32_t Levels = 5;       // light_shadow.h SceneMaps::PyramidLevels
    static constexpr uint32_t HeaderWords = 16; // ... PyramidHeaderWords

    VrDepthPyramid(const Instance& instance, Scheduler& scheduler);
    ~VrDepthPyramid();

    // Recording thread, outside a render pass: this frame's pyramid of `left` and `right` (the
    // eyes' views of the AO pass's depth, level 0 `width` x `height`, readable in `layout`).
    // False when it cannot be made (no pipeline or buffer): the march then reads no pyramid.
    bool Record(const RecorderProxy& cmd, vk::ImageView left, vk::ImageView right,
                vk::ImageLayout layout, uint32_t width, uint32_t height);
    // The buffer and the bytes the last Record wrote (the AO pass binds them).
    vk::Buffer Buffer() const {
        return buffer;
    }
    uint64_t Bytes() const {
        return used;
    }
    static uint64_t BytesFor(uint32_t width, uint32_t height);
    // The buffer given back (the lamp shadows off a while: Rasterizer::VrLampShadowsRelease),
    // freed once the GPU is past this frame; the next Record makes it again. The recording thread.
    void Release();

private:
    bool Init();
    const Instance& instance;
    Scheduler& scheduler;
    bool tried{};
    bool ready{};
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::Buffer buffer{};
    VmaAllocation_T* memory{};
    uint64_t capacity{}, used{};
    uint32_t counter{};
};

} // namespace Vulkan
