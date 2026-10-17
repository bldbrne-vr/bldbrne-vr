// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_perf_memo.h"

namespace Vulkan {

// Bit 58 (bbport PreparedStage image/sampler/buffer sharps and image hashes,
// BBV vk_draw_prep.h:33-48): a stage's resource sharps, texture hashes and bind-memo key, built
// by a preparation worker over its private Info copy (the flat data and user data Stage A
// compares in TryPreparedGraphics / PacketReuse). Stage A hands them to B by view instead of
// building Rasterizer::PreparedResources itself (bit 13). Built exactly as
// PreparedResources::Build does; FAST_RESOURCE_PREP_VERIFY and FAST_PIPE_VERIFY compare.
struct PreparedBindingImage {
    AmdGpu::Image sharp;
    u64 hash;
};
static_assert(sizeof(PreparedBindingImage) == 40 &&
              std::is_trivially_copyable_v<PreparedBindingImage>);

inline u64 PreparedImageHash(const std::array<u64, 4>& raw, u32 flags) {
    u64 h = raw[0] * 0x9E3779B97F4A7C15ull;
    h ^= (raw[1] + flags) * 0xC2B2AE3D27D4EB4Full;
    h ^= raw[2] * 0x165667B19E3779F9ull;
    h ^= raw[3] * 0x27D4EB2F165667C5ull;
    return h ^ (h >> 29);
}

// One stage's record in the submission arena, 8-byte aligned throughout: this header, then
// PreparedBindingImage[num_images], AmdGpu::Sampler[num_samplers], AmdGpu::Buffer[num_buffers]
// and u64[key_words] (each array padded to 8 bytes).
struct PreparedBindingStage {
    u32 num_images, num_samplers, num_buffers, key_words;
    u64 key_hash;
    u32 keyed, bytes; // bytes: the whole record, header included
};
static_assert(sizeof(PreparedBindingStage) == 32);

constexpr size_t PreparedBindingPad(size_t bytes) {
    return (bytes + 7) & ~size_t{7};
}

// Worker side. False: this stage cannot be prepared (B builds it, or bit 13's A path does).
inline bool AppendPreparedBindings(const Shader::Info& stage, std::vector<std::byte>& blob,
                                   std::vector<PreparedBindingImage>& images,
                                   PerfMemo::BindKey& key) {
    images.clear();
    key.Clear();
    bool keyed = !stage.images.empty();
    for (const auto& desc : stage.images) {
        const auto sharp = desc.GetSharp(stage);
        std::array<u64, 4> raw;
        static_assert(sizeof(raw) == sizeof(sharp));
        std::memcpy(raw.data(), &sharp, sizeof(raw));
        const u32 flags =
            u32(desc.is_written) | u32(desc.is_depth) << 1 | u32(desc.is_array) << 2;
        images.push_back({sharp, PreparedImageHash(raw, flags)});
        keyed &= !desc.is_written;
        key.Add(sharp, PerfMemo::BindAttrs(desc.is_depth, desc.is_array, desc.is_written,
                                          static_cast<u32>(desc.mip_fallback_mode),
                                          desc.constant_mip_index));
    }
    keyed &= key.Usable();
    const size_t image_bytes = PreparedBindingPad(images.size() * sizeof(PreparedBindingImage));
    const size_t sampler_bytes = PreparedBindingPad(stage.samplers.size() * sizeof(AmdGpu::Sampler));
    const size_t buffer_bytes = PreparedBindingPad(stage.buffers.size() * sizeof(AmdGpu::Buffer));
    const u32 key_words = keyed ? key.count * PerfMemo::BindKey::WordsPerImage : 0;
    const size_t bytes = sizeof(PreparedBindingStage) + image_bytes + sampler_bytes + buffer_bytes +
                         size_t(key_words) * sizeof(u64);
    const size_t start = blob.size();
    blob.resize(start + bytes, std::byte{});
    std::byte* out = blob.data() + start;
    PreparedBindingStage header{};
    header.num_images = static_cast<u32>(images.size());
    header.num_samplers = static_cast<u32>(stage.samplers.size());
    header.num_buffers = static_cast<u32>(stage.buffers.size());
    header.key_words = key_words;
    header.key_hash = keyed ? key.Hash() : 0;
    header.keyed = keyed ? 1u : 0u;
    header.bytes = static_cast<u32>(bytes);
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    if (!images.empty())
        std::memcpy(out, images.data(), images.size() * sizeof(PreparedBindingImage));
    out += image_bytes;
    for (size_t i = 0; i < stage.samplers.size(); ++i) {
        const auto& desc = stage.samplers[i];
        auto sharp = desc.GetSharp(stage);
        if (desc.disable_aniso) {
            if (desc.associated_image >= images.size()) {
                blob.resize(start);
                return false; // PreparedResources::Build asserts: leave it to that path
            }
            const auto& image = images[desc.associated_image].sharp;
            if (image.base_level == 0 && image.last_level == 0)
                sharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
        }
        std::memcpy(out + i * sizeof(AmdGpu::Sampler), &sharp, sizeof(sharp));
    }
    out += sampler_bytes;
    for (size_t i = 0; i < stage.buffers.size(); ++i) {
        const auto sharp = stage.buffers[i].GetSharp(stage);
        std::memcpy(out + i * sizeof(AmdGpu::Buffer), &sharp, sizeof(sharp));
    }
    out += buffer_bytes;
    if (key_words)
        std::memcpy(out, key.words.data(), size_t(key_words) * sizeof(u64));
    return true;
}

// Stage A side: one stage's record at `at` (8-byte aligned, inside the submission arena).
struct PreparedBindingView {
    std::span<const PreparedBindingImage> images;
    std::span<const AmdGpu::Sampler> samplers;
    std::span<const AmdGpu::Buffer> buffers;
    std::span<const u64> key_words;
    u64 key_hash{};
    bool keyed{};
    u32 bytes{};
};
inline PreparedBindingView ReadPreparedBindings(const std::byte* at) {
    PreparedBindingStage header;
    std::memcpy(&header, at, sizeof(header));
    PreparedBindingView view;
    const std::byte* p = at + sizeof(header);
    view.images = {reinterpret_cast<const PreparedBindingImage*>(p), header.num_images};
    p += PreparedBindingPad(header.num_images * sizeof(PreparedBindingImage));
    view.samplers = {reinterpret_cast<const AmdGpu::Sampler*>(p), header.num_samplers};
    p += PreparedBindingPad(header.num_samplers * sizeof(AmdGpu::Sampler));
    view.buffers = {reinterpret_cast<const AmdGpu::Buffer*>(p), header.num_buffers};
    p += PreparedBindingPad(header.num_buffers * sizeof(AmdGpu::Buffer));
    view.key_words = {reinterpret_cast<const u64*>(p), header.key_words};
    view.key_hash = header.key_hash;
    view.keyed = header.keyed != 0;
    view.bytes = header.bytes;
    return view;
}

} // namespace Vulkan
