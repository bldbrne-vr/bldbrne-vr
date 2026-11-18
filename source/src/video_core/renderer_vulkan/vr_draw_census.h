// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_DRAW_CENSUS=1 (diagnostic, off by default): where a frame's draws go. Each stage B draw
// (DrawRecord, after BeginRendering) is counted under its render pass's shape: the target size,
// the colour attachment count, a depth attachment, the view mask (3: both eyes, multiview).
// Every 240 frames: `VR draw census:` with draws per frame and the busiest shapes. FrameEnd is
// called from FlushFrameEnd (stage B's thread, as every Note).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"

namespace Vulkan::DrawCensus {

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("VR_DRAW_CENSUS");
        return v && v[0] == '1';
    }();
    return on;
}

struct Shape {
    u16 width, height, colors;
    bool depth;
    u32 views;
    bool operator==(const Shape&) const = default;
};
struct ShapeHash {
    size_t operator()(const Shape& s) const noexcept {
        return (u64(s.width) << 40) ^ (u64(s.height) << 24) ^ (u64(s.colors) << 16) ^
               (u64(s.depth) << 8) ^ s.views;
    }
};

class Census {
public:
    // `cell`: the shadow atlas's 4x4 grid cell (row * 4 + col) of a square 2048..8192 depth
    // target's draw, else -1. `vs` / `ps`: the draw's guest shader hashes (for the shapes the
    // detail lines name).
    void Note(u16 width, u16 height, u16 colors, bool depth, u32 views, int cell, u64 vs, u64 ps) {
        const Shape shape{width, height, colors, depth, views};
        ++counts[shape];
        last = shape;
        has_last = true;
        ++draws;
        if (cell >= 0 && cell < 16) {
            ++cells[cell];
        }
        // The half-size passes and the full-size single-target passes: which shaders draw them.
        if ((colors == 1 && depth) || (width * 2 <= 1600 && depth)) {
            ++shaders[{shape, vs, ps}];
        }
    }
    // The stage B cycles of the draw Note last counted (DrawRecord's whole body).
    void AddTicks(u64 cycles) {
        if (has_last) {
            ticks[last] += cycles;
            has_last = false;
        }
    }
    void FrameEnd() {
        if (++frames < 240) {
            return;
        }
        std::vector<std::pair<Shape, u64>> order(counts.begin(), counts.end());
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string top;
        for (size_t i = 0; i < order.size() && i < 16; ++i) {
            const auto& [s, n] = order[i];
            const double us = double(ticks[s]) / 3400.0 / double(frames); // ~3.4 GHz TSC
            top += fmt::format(" | {}x{} c{}{}{} {:.1f}/f {:.0f}us/f {:.2f}us/draw", s.width,
                               s.height, s.colors, s.depth ? " d" : "",
                               s.views == 3 ? " both-eyes" : "", double(n) / double(frames), us,
                               n ? us * double(frames) / double(n) : 0.0);
        }
        LOG_INFO(Render_Vulkan, "VR draw census: {:.0f} draws/f in {} shapes{}",
                 double(draws) / double(frames), counts.size(), top);
        std::string atlas;
        for (int c = 0; c < 16; ++c) {
            if (cells[c]) {
                atlas += fmt::format(" cell {},{} {:.1f}/f", c / 4, c % 4,
                                     double(cells[c]) / double(frames));
            }
        }
        LOG_INFO(Render_Vulkan, "VR draw census: shadow atlas by cell (row,col):{}", atlas);
        std::vector<std::pair<ShaderKey, u64>> by_shader(shaders.begin(), shaders.end());
        std::sort(by_shader.begin(), by_shader.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string detail;
        for (size_t i = 0; i < by_shader.size() && i < 14; ++i) {
            const auto& [k, n] = by_shader[i];
            detail += fmt::format(" | {}x{} c{}{} vs {:x} ps {:x} {:.1f}/f", k.shape.width,
                                  k.shape.height, k.shape.colors, k.shape.depth ? " d" : "",
                                  k.vs, k.ps, double(n) / double(frames));
        }
        LOG_INFO(Render_Vulkan, "VR draw census: single-target and half-size passes by shader:{}",
                 detail);
        counts.clear();
        ticks.clear();
        shaders.clear();
        cells = {};
        draws = frames = 0;
    }

private:
    struct ShaderKey {
        Shape shape;
        u64 vs, ps;
        bool operator==(const ShaderKey&) const = default;
    };
    struct ShaderKeyHash {
        size_t operator()(const ShaderKey& k) const noexcept {
            return ShapeHash{}(k.shape) ^ (k.vs * 0x9E3779B97F4A7C15ull) ^ (k.ps << 1);
        }
    };
    std::unordered_map<Shape, u64, ShapeHash> counts, ticks;
    Shape last{};
    bool has_last{};
    std::unordered_map<ShaderKey, u64, ShaderKeyHash> shaders;
    std::array<u64, 16> cells{};
    u64 draws{}, frames{};
};

// Stage B's own (one recording thread).
inline Census& Get() {
    static Census census;
    return census;
}

} // namespace Vulkan::DrawCensus
