// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocation_T;

namespace Vulkan {
class Instance;
class Scheduler;
class RecorderProxy;

// The hunter's real silhouette for the lamp shadows (2026-11-12, the user: "Why can't we reuse
// the actual hunter's draws and instead we added the capsule thing?", then "I need it fast and
// performant"). The game draws his whole posed body (hat, coat, weapons) into its moon shadow
// map every frame. In that pass's nearest cascade each draw of his takes a capture variant of its
// pipeline (Shader::VrMotion::Capture): its vertex shader also stores each vertex's clip position
// here, w the frame's tag. Before the frame's first lamp draw one compute pass
// (host_shaders/vr_body_maps.comp) turns his triangles back into the world with that pass's own
// camera (its 864-byte camera buffer: the cascade's view and orthographic projection) and draws
// them, from each of up to MaxMaps lamps whose light reaches him, into a small depth map. The lamp
// shadows test a pixel against its lamp's map (emit_spirv_light_shadow.cpp); a lamp without one
// keeps the capsules (body_shadow.h), so does a frame whose capture is incomplete.
// No game draw is issued again, nothing waits: a few small buffers, one dispatch, ~0.6 MB of VRAM
// for the maps and positions, 1 MiB of index pool in host memory.
class VrBodyMaps {
public:
    static constexpr uint32_t MaxDraws = 64;
    static constexpr uint32_t MaxMaps = 2;
    static constexpr uint32_t MapSize = 256; // Shader::Optimization::BodyShadow::MapTexels
    static constexpr uint32_t MaxVertices = 40960;
    static constexpr uint32_t Slots = 4; // per-frame host data in flight
    static constexpr uint32_t PoolBytes = 1u << 20;
    static constexpr uint32_t TableMapDwords = 16, TableDrawDwords = 8;
    static constexpr uint32_t TableDwords = MaxMaps * TableMapDwords + MaxDraws * TableDrawDwords;

    // One captured draw of his (its mesh's indices in the pool, PoolIndices).
    struct Draw {
        uint32_t pool_first{}; // first index in the pool
        uint32_t count{};      // indices
        bool short_indices{};
        uint32_t lowest{}, vertices{}; // the index range it stores positions for
        uint32_t vertex_offset{};      // added to every index (the draw's base vertex)
        uint32_t first_instance{};     // the draw's first instance (its InstanceIndex)
        // Cascade clip to the world (relative to his origin), rows of a 3x4 (orthographic).
        std::array<float, 12> clip_to_world{};
    };
    // A lamp for a map: its frame in the world (relative to his origin), lamp at `origin`, the
    // map's axes as rows, `f` the projection's scale (1 / tan of the half angle), `near` (game
    // units: nearer is left out, the hand that holds a torch).
    struct Lamp {
        std::array<double, 3> origin{};
        std::array<double, 9> axes{};
        float f{}, near_z{};
    };
    // A map built for a frame: its lamp, and the first texel of its map.
    struct Built {
        bool valid{};
        Lamp lamp{};
        uint32_t base{};
    };

    VrBodyMaps(const Instance& instance, Scheduler& scheduler);
    ~VrBodyMaps();

    // Recording thread: makes the buffers and the pipeline (once; false when it cannot).
    bool Prepare() {
        return Init();
    }
    // Made (any thread).
    bool Ready() const {
        return ready.load(std::memory_order_acquire);
    }

    // Producer side (VrCapeSkip, its thread only): the pool's first index of this mesh's indices
    // (copied once; `count` indices of `index_size` bytes), or ~0u when the pool is full or it
    // cannot be made.
    uint32_t PoolIndices(const uint8_t* data, uint32_t count, uint32_t index_size);

    // Recording thread. A capture draw of `frame`: its parameters written, `param` the pair index
    // to push and the buffers' addresses; false when this frame cannot take it (full, failed).
    bool AddDraw(uint64_t frame, const Draw& draw, uint32_t& param, uint64_t& params_address,
                 uint64_t& positions_address);
    // The buffers' addresses with entry 0, which stores nothing (a capture draw the frame cannot
    // take); zeros before the buffers exist (never asked then: Ready gates the variant).
    void Disabled(uint64_t& params_out, uint64_t& positions_out) const {
        params_out = params_address;
        positions_out = positions_address;
    }
    // A draw of his in the cascade that could not be captured (its variant still compiling): the
    // frame's maps are left out (never a body with a part missing).
    void Missed(uint64_t frame);
    // Before `frame`'s first lamp draw, outside a render pass: its maps, one per lamp (up to
    // MaxMaps). False: nothing recorded (no capture, incomplete, no lamp).
    bool Record(const RecorderProxy& cmd, uint64_t frame, std::span<const Lamp> lamps);
    // The maps built for `frame` (valid false when none).
    std::array<Built, MaxMaps> Maps(uint64_t frame) const;
    // The maps' buffer and its size (the lamp shadows bind it), null before the first.
    vk::Buffer MapsBuffer() const {
        return maps;
    }
    static constexpr uint64_t MapsBytes() {
        return uint64_t(MaxMaps) * MapSize * MapSize * sizeof(uint32_t);
    }

private:
    bool Init();
    const Instance& instance;
    Scheduler& scheduler;
    bool tried{};
    std::atomic<bool> ready{};
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    // Host-visible: the parameters (Slots x MaxDraws, VrMotion::Params), the tables, the pool.
    vk::Buffer params{}, positions{}, tables{}, pool{}, maps{};
    VmaAllocation_T *params_memory{}, *positions_memory{}, *tables_memory{}, *pool_memory{},
        *maps_memory{};
    uint64_t params_address{}, positions_address{};
    void* params_mapped{};
    void* tables_mapped{};
    uint8_t* pool_mapped{};
    // Producer: bytes of the pool used (append only: a mesh's indices never move).
    uint32_t pool_used{};
    // Recording: the frame being captured.
    uint64_t frame{~0ull};
    uint32_t counter{}; // frames captured (the tag, the slot)
    uint32_t tag{};
    bool missed{};
    std::vector<Draw> draws;
    uint32_t vertices{};
    std::array<Built, MaxMaps> built{};
    uint64_t built_frame{~0ull};
    // VR_BODY_MAPS_DUMP=1 (tests): the maps, every 300th built, copied to `dump` and written to
    // user/log/body_maps_<n>.bin once the GPU is done (float32 lamp z per texel, MaxMaps maps).
    vk::Buffer dump{};
    VmaAllocation_T* dump_memory{};
    void* dump_mapped{};
    uint32_t built_count{};
};

} // namespace Vulkan
