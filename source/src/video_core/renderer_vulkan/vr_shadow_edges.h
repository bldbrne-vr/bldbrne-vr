// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocation_T;

namespace Vulkan {
class Instance;
class Scheduler;
class RecorderProxy;

// The lamp shadows' edge pass (2026-11-19, the user: "make the adaptive edges faster than before
// but higher quality"; shader_recompiler/ir/passes/light_shadow.h SceneMaps::EdgePass). Before the
// frame's first lamp draw, one dispatch (host_shaders/vr_shadow_edges.comp) marches the frame's
// edge lamps where the half-res mask's neighbours disagree, both eyes at the lamp draws' size, on
// the depth those draws read, and writes two bits a lamp a pixel. The lamp shaders read one word a
// pixel in place of marching themselves (their march's code had cost every lamp pixel).
class VrShadowEdges {
public:
    static constexpr uint32_t ParamWords = 160; // light_shadow.h SceneMaps::EdgeParamWords
    static constexpr uint32_t DataAt = ParamWords + 4;

    VrShadowEdges(const Instance& instance, Scheduler& scheduler);
    ~VrShadowEdges();

    // The buffer's first DataAt words: `params` (vr_shadow_edges.comp's layout), then the header
    // (serial, width, height, words a row).
    // (`bits`: a pixel's, 8 or 16: light_shadow.h SceneMaps::EdgeBits; the header's words a row.)
    static void FillHead(std::span<uint32_t, DataAt> head, std::span<const uint32_t, ParamWords> params,
                         uint32_t serial, uint32_t width, uint32_t height, uint32_t bits);
    // Recording thread, outside a render pass: this frame's pass. `head`: FillHead's words, in a
    // buffer the GPU reads (the stream buffer), copied in by a recorded copy (a raw command buffer
    // drained the recorder once a frame: a code review, 2026-11-20). `left`/`right`: the eyes'
    // views of the lamp draws' depth, `width` x `height`, readable in `layout`; `mask`: the shadow
    // mask's buffer and bytes. False when it cannot run.
    bool Record(const RecorderProxy& cmd, vk::ImageView left, vk::ImageView right,
                vk::ImageLayout layout, uint32_t width, uint32_t height, vk::Buffer mask,
                uint64_t mask_bytes, vk::Buffer head, uint64_t head_offset, uint32_t bits);
    vk::Buffer Buffer() const {
        return buffer;
    }
    uint64_t Bytes() const {
        return used;
    }
    static uint32_t WordsARow(uint32_t width, uint32_t bits) { // (a pixel's bits: 8 or 16)
        const uint32_t per = bits == 16 ? 2u : 4u;
        return (width + per - 1) / per;
    }
    static uint64_t BytesFor(uint32_t width, uint32_t height, uint32_t bits) {
        return (uint64_t(DataAt) + 2ull * height * WordsARow(width, bits)) * sizeof(uint32_t);
    }
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
};

} // namespace Vulkan
