// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include "shader_recompiler/vr_motion.h"
#include "video_core/renderer_vulkan/vr_motion_history.h"
namespace Vulkan {
// Renderer-owned, constructed lazily only after a temporal provider is ready. Both eyes
// share draw matching (the native multiview draw is identical) but never position storage.
// No per-eye duplicate hashing, no per-draw allocation after the bbport tables warm up.
class VrObjectMotionState {
public:
    static constexpr uint32_t FrameSlots = 4, DrawsPerFrame = 8192;
    static constexpr uint32_t ParameterCount = 2 + 2 * FrameSlots * DrawsPerFrame;
    static_assert(ParameterCount - 1 <= Shader::VrMotion::MaxParameterIndex);
    explicit VrObjectMotionState(uint32_t capacity) : capacity(capacity), history(capacity) {
        assert(capacity && uint64_t(capacity) * 4 + 2 <= UINT32_MAX);
    }
    bool Begin(uint64_t frame_id, uint64_t epoch, uint64_t completed, bool enabled,
               bool camera_cut = false) {
        active = false;
        if (!enabled || (seen && frame_id <= last_frame))
            return false;
        slot = uint32_t(frame_id % FrameSlots);
        if (ticks[slot] > completed)
            return false; // Caller may skip; never silently overwrite.
        const bool reset = !seen || frame_id - last_frame != 1 || epoch != last_epoch || camera_cut;
        if (reset)
            history.NextFrame(); // Drop history while retaining warm table storage.
        history.NextFrame();
        completed_tick = completed;
        used = 0;
        last_frame = frame_id;
        last_epoch = epoch;
        seen = true;
        active = true;
        return true;
    }
    struct Prepared {
        uint32_t index{};
        std::array<Shader::VrMotion::Params, 2> eyes{};
    };
    Prepared Prepare(const Motion::Draw& draw, uint64_t ticket) {
        if (!active || used >= DrawsPerFrame || ticket <= completed_tick)
            return {};
        const auto a = history.Prepare(draw);
        if (!a.store && !a.load)
            return {};
        Prepared result;
        result.index = 2 + 2 * (slot * DrawsPerFrame + used++);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const uint32_t base = eye * (2 * capacity + 1);
            result.eyes[eye] = {a.store ? base + a.store : 0,
                                a.load ? base + a.load : 0,
                                a.vertices,
                                (a.store ? Shader::VrMotion::FlagStore : 0u) |
                                    (a.load ? Shader::VrMotion::FlagLoad : 0u),
                                a.first_vertex,
                                a.first_instance,
                                a.instances,
                                0};
        }
        ticks[slot] = std::max(ticks[slot], ticket);
        return result;
    }
    uint64_t ReuseTicket(uint64_t frame_id) const {
        return ticks[frame_id % FrameSlots];
    }
    uint32_t Used() const { return history.Used(); }
    uint64_t PositionBytes() const {
        return (uint64_t(capacity) * 4 + 2) * 16;
    }
    const Motion::History::Stats& Stats() const {
        return history.stats;
    }

private:
    uint32_t capacity{}, slot{}, used{};
    Motion::History history;
    std::array<uint64_t, FrameSlots> ticks{};
    uint64_t last_frame{}, last_epoch{}, completed_tick{};
    bool seen{}, active{};
};
} // namespace Vulkan
