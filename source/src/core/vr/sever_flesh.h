// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Beast Claw dismemberment, the flesh as a texture: src/video_core/host_shaders/vr_flesh.glsl's
// albedo ported line by line (the same hash, noise, bands), baked once into a small RGBA8 tile
// with its mips. The in-frame pass draws a severed piece and its caps as game models (their own
// texture, the scene's light): a cap samples this tile at TexCoord(its flesh uv). The shader's
// bend and wetness need a pass of their own; this is the albedo. Pure: no Vulkan, no game memory.
namespace VR::Sever::Flesh {

inline float Fract(float x) {
    return x - std::floor(x);
}

// Hoskins' hash12 (vr_flesh.glsl FleshHash).
inline float Hash(float px, float py) {
    float q[3] = {Fract(px * 0.1031f), Fract(py * 0.1031f), Fract(px * 0.1031f)};
    const float d = q[0] * (q[1] + 33.33f) + q[1] * (q[2] + 33.33f) + q[2] * (q[0] + 33.33f);
    for (float& v : q) {
        v += d;
    }
    return Fract((q[0] + q[1]) * q[2]);
}

// Smooth value noise (FleshNoise).
inline float Noise(float px, float py) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

inline float Smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// FleshSurface's albedo (linear) at a cap's flesh uv (the bone at (0, 0), the ring's mean radius 1).
// The meat's colour, darkened (2026-11-13: "darken a bit the red color we use at the
// fleshy fill", then "make it a bit darker", then "darker red.. more!"): x0.35, linear (about 40%
// darker on screen than the first red). The bone and its marrow keep theirs.
inline constexpr float MeatShade = 0.35f;

inline std::array<float, 3> Albedo(float u, float v, float fresh) {
    const float r = std::sqrt(u * u + v * v);
    const float fibres = Noise(u * 18.0f, v * 5.0f);
    const float clots = Noise(u * 6.0f + 17.0f, v * 6.0f + 17.0f);
    const float meat = 0.65f * fibres + 0.35f * clots;
    std::array<float, 3> a{0.13f + (0.40f - 0.13f) * meat, 0.008f + (0.030f - 0.008f) * meat,
                           0.007f + (0.024f - 0.007f) * meat};
    for (float& x : a) {
        x *= (1.0f + 0.6f * fresh) * MeatShade;
    }
    const float bone = 1.0f - Smooth(0.16f, 0.20f, r);
    const float marrow = 1.0f - Smooth(0.05f, 0.08f, r);
    const std::array<float, 3> bone_c{0.70f, 0.60f, 0.46f}, marrow_c{0.18f, 0.025f, 0.015f};
    const float shade = 0.8f + 0.2f * clots;
    for (int i = 0; i < 3; ++i) {
        a[i] += (bone_c[i] * shade - a[i]) * bone;
        a[i] += (marrow_c[i] - a[i]) * marrow;
        a[i] *= 1.0f + (0.3f - 1.0f) * Smooth(1.5f, 3.0f, r);
    }
    return a;
}

// The tile spans flesh uv -Reach .. Reach on both axes: a cap's far rim, its bone off its middle,
// stays on it (2026-11-16: at 1.6, 21 - 58% of cap points were clamped, a smear of the edge's
// texels). TileSize keeps the old texel density (about 85 a radius).
inline constexpr float Reach = 3.0f;
inline constexpr std::uint32_t TileSize = 512;
inline std::array<float, 2> TexCoord(float u, float v) {
    return {std::clamp(0.5f + u / (2 * Reach), 0.004f, 0.996f),
            std::clamp(0.5f + v / (2 * Reach), 0.004f, 0.996f)};
}

inline std::uint8_t Srgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}

// The tile: `size` (a power of two) square, RGBA8 sRGB, its mips down to 1x1 (each the average of
// the four texels above it), level after level in `rgba`, each level's byte offset in `offsets`.
struct Tile {
    std::uint32_t size{0}, levels{0};
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint8_t> rgba;
};
inline Tile Bake(std::uint32_t size = TileSize, float fresh = 0.6f) {
    Tile t;
    if (size == 0 || (size & (size - 1)) != 0 || size > 2048) {
        return t;
    }
    t.size = size;
    std::vector<float> lin(static_cast<std::size_t>(size) * size * 3);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = ((x + 0.5f) / size - 0.5f) * 2 * Reach;
            const float v = ((y + 0.5f) / size - 0.5f) * 2 * Reach;
            const auto a = Albedo(u, v, fresh);
            for (int c = 0; c < 3; ++c) {
                lin[(static_cast<std::size_t>(y) * size + x) * 3 + c] = a[c];
            }
        }
    }
    for (std::uint32_t s = size;; s /= 2) {
        t.offsets.push_back(static_cast<std::uint32_t>(t.rgba.size()));
        for (std::size_t i = 0; i < static_cast<std::size_t>(s) * s; ++i) {
            t.rgba.push_back(Srgb(lin[i * 3]));
            t.rgba.push_back(Srgb(lin[i * 3 + 1]));
            t.rgba.push_back(Srgb(lin[i * 3 + 2]));
            t.rgba.push_back(255);
        }
        ++t.levels;
        if (s == 1) {
            break;
        }
        const std::uint32_t h = s / 2;
        std::vector<float> next(static_cast<std::size_t>(h) * h * 3);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < h; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const auto at = [&](std::uint32_t xx, std::uint32_t yy) {
                        return lin[(static_cast<std::size_t>(yy) * s + xx) * 3 + c];
                    };
                    next[(static_cast<std::size_t>(y) * h + x) * 3 + c] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                                 at(2 * x + 1, 2 * y + 1));
                }
            }
        }
        lin = std::move(next);
    }
    return t;
}

} // namespace VR::Sever::Flesh
