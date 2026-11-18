// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include "core/vr/bloodborne_assets.h"

namespace VR::AnimatedItem {
// Validates the complete embedded payload before publishing it to the renderer.
template <int Model, unsigned AtlasSize, unsigned AtlasLevels, unsigned MaxVertices,
          unsigned MaxIndices, bool SingleSurface = false, unsigned Bones = 16>
inline std::shared_ptr<Assets::Model> Decode(std::span<const u8> data) {
    constexpr unsigned Frames = 255;
    static_assert(Bones > 0 && Bones <= 32);
    constexpr unsigned TextureBytes = [] {
        unsigned bytes = 0;
        for (unsigned i = 0; i < AtlasLevels; ++i)
            bytes += (AtlasSize >> i) * (AtlasSize >> i);
        return bytes;
    }();
    if (data.size() < 64)
        return {};
    u32 h[10];
    float bounds[6];
    std::memcpy(h, data.data(), sizeof(h));
    std::memcpy(bounds, data.data() + sizeof(h), sizeof(bounds));
    const u32 nv = h[2], ni = h[3], anim = h[8], bytes = h[9];
    if (h[0] != 0x31524242 || h[1] != 2 || nv == 0 || nv > MaxVertices || ni == 0 ||
        ni > MaxIndices || ni % 3 || h[4] != Frames || h[5] != Bones || h[6] != AtlasSize ||
        h[7] != AtlasLevels || anim != 2 * nv + Frames * Bones * 3 || bytes != TextureBytes)
        return {};
    const size_t expected =
        64 + size_t(nv) * 32 + size_t(ni) * 4 + size_t(anim) * 16 + size_t(bytes) * 2;
    if (data.size() != expected)
        return {};
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(bounds[i]) || !std::isfinite(bounds[i + 3]) ||
            bounds[i] >= bounds[i + 3] || std::abs(bounds[i]) > 2 || std::abs(bounds[i + 3]) > 2)
            return {};
    auto model = std::make_shared<Assets::Model>();
    model->id = Model;
    model->single_surface = SingleSurface;
    model->animation_bones = Bones;
    size_t at = 64;
    const auto copy = [&](void* dst, size_t n) {
        std::memcpy(dst, data.data() + at, n);
        at += n;
    };
    model->vertices.resize(nv);
    copy(model->vertices.data(), size_t(nv) * 32);
    model->indices.resize(ni);
    copy(model->indices.data(), size_t(ni) * 4);
    model->animation.resize(size_t(anim) * 4);
    copy(model->animation.data(), size_t(anim) * 16);
    for (const auto& v : model->vertices) {
        for (float x : v.position)
            if (!std::isfinite(x) || std::abs(x) > 2)
                return {};
        for (float x : v.normal)
            if (!std::isfinite(x) || std::abs(x) > 1.01f)
                return {};
        for (float x : v.uv)
            if (!std::isfinite(x) || std::abs(x) > 64)
                return {};
    }
    for (u32 i : model->indices)
        if (i >= nv)
            return {};
    for (float x : model->animation)
        if (!std::isfinite(x))
            return {};
    for (u32 i = 0; i < nv; i++) {
        const float* w = model->animation.data() + 4 * i;
        if (w[0] < 0 || w[0] >= Bones || w[1] < 0 || w[1] >= Bones || w[0] != std::floor(w[0]) ||
            w[1] != std::floor(w[1]) || w[2] < 0 || w[3] < 0 || std::abs(w[2] + w[3] - 1) > 1e-4f)
            return {};
        const float* color = model->animation.data() + 4 * (nv + i);
        for (unsigned c = 0; c < 4; ++c)
            if (color[c] < 0.0f || color[c] > 4.0f)
                return {};
        if (model->vertices[i].uv[0] < 0.0f ||
            model->vertices[i].uv[0] >= (SingleSurface ? 16.0f : 64.0f))
            return {};
    }
    // A triangle must stay in one material tile: interpolating a tile id across its face
    // would produce incorrect sampling and derivatives even though the indices are in range.
    for (size_t i = 0; i < ni; i += 3) {
        const int tile = static_cast<int>(model->vertices[model->indices[i]].uv[0] / 16.0f);
        for (size_t j = 1; j < 3; ++j)
            if (static_cast<int>(model->vertices[model->indices[i + j]].uv[0] / 16.0f) != tile)
                return {};
    }
    for (size_t i = size_t(nv) * 8; i < model->animation.size(); ++i)
        if (std::abs(model->animation[i]) > 4.0f)
            return {};
    for (auto* t : {&model->diffuse, &model->surface}) {
        t->format = Assets::ModelTexture::Format::Bc3;
        t->width = t->height = AtlasSize;
        t->levels = AtlasLevels;
        u32 offset = 0;
        for (u32 l = 0; l < AtlasLevels; l++) {
            t->level_offset.push_back(offset);
            offset += (AtlasSize >> l) * (AtlasSize >> l); // BC3: one byte per texel
        }
        t->blocks.resize(bytes);
        copy(t->blocks.data(), bytes);
    }
    std::copy_n(bounds, 3, model->min);
    std::copy_n(bounds + 3, 3, model->max);
    return model;
}
} // namespace VR::AnimatedItem
