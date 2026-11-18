// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_set_write_codec.h"

namespace Vulkan::StereoSetCodec {

/// VR_COMPACT_SETS (Pipeline::BindResources, CommandRecorder::WriteDescriptorSetCompact): a
/// native stereo descriptor set written from the draw's unpaired writes and, per paired info, only
/// what the right eye takes. The replay expands it into the writes NativeStereo's pairing makes
/// (Fused::Pair): every storage buffer info becomes [left, right] with the right buffer (or the
/// info's own camera pair's right info), every sampled or storage image info [left, right] with
/// the right view; other writes stay as they are. Stage B then copies each info once, not the
/// pairing's two copies of the doubled infos plus the codec's.
///
/// Encoding: SetWriteCodec's (an Entry per write with its unpaired count, then every write's
/// infos by value), then an 8-byte right handle per paired info in write order (the right buffer
/// for a storage buffer, the right view for an image), then the camera table: a u32 count, a u32
/// pad, and per entry a u32 index among the storage buffer infos, a u32 pad and the right info.

/// Paired as Fused::Pair pairs (NativeStereo's CarriedOps::KindOf).
enum class Kind : u8 { Buffer, Image, Other };
inline Kind KindOf(vk::DescriptorType type) noexcept {
    switch (type) {
    case vk::DescriptorType::eStorageBuffer:
        return Kind::Buffer;
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
        return Kind::Image;
    default:
        return Kind::Other;
    }
}

struct Camera {
    u32 index; // among the storage buffer infos, in write order
    u32 pad;
    vk::DescriptorBufferInfo right;
};
static_assert(sizeof(Camera) == 32);

/// What the pairing gives the right eye (NativeStereo::CompactRights fills it).
struct Rights {
    std::vector<u64> handles;   // per paired info, in write order
    std::vector<Camera> cameras; // storage buffer infos whose right is their camera pair's
    u32 paired{};               // handles used
    u32 cameras_used{};
};

/// The encoding's size, or 0 when a write cannot be carried (SetWriteCodec::PayloadBytes).
inline u32 EncodedBytes(std::span<const vk::WriteDescriptorSet> writes, const Rights& rights) {
    const u32 base = SetWriteCodec::EncodedBytes(writes);
    if (base == 0) {
        return 0;
    }
    return base + rights.paired * u32{sizeof(u64)} + 8 + rights.cameras_used * u32{sizeof(Camera)};
}

/// Writes the encoding (EncodedBytes non-zero) to `dst`.
inline void Encode(std::byte* dst, std::span<const vk::WriteDescriptorSet> writes,
                   const Rights& rights) {
    const u32 base = SetWriteCodec::EncodedBytes(writes);
    SetWriteCodec::Encode(dst, writes);
    std::byte* tail = dst + base;
    std::memcpy(tail, rights.handles.data(), size_t{rights.paired} * sizeof(u64));
    tail += size_t{rights.paired} * sizeof(u64);
    const u32 table[2]{rights.cameras_used, 0};
    std::memcpy(tail, table, sizeof(table));
    tail += sizeof(table);
    std::memcpy(tail, rights.cameras.data(), size_t{rights.cameras_used} * sizeof(Camera));
}

/// The replay's scratch: the expanded writes and the infos they point into.
struct Expanded {
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorBufferInfo> buffers;
    std::vector<vk::DescriptorImageInfo> images;
};

/// The `count` writes encoded at `src` (`bytes` long) expanded into `out` (dstSet null). False
/// when the encoding does not hold together (its counts run past its end): nothing is written.
inline bool Decode(const std::byte* src, u32 bytes, u32 count, Expanded& out) {
    out.writes.resize(count);
    SetWriteCodec::Decode(src, count, out.writes.data());
    size_t buffer_infos = 0, image_infos = 0, paired = 0;
    u32 base = static_cast<u32>(size_t{count} * sizeof(SetWriteCodec::Entry));
    for (const auto& w : out.writes) {
        base += SetWriteCodec::PayloadBytes(w);
        switch (KindOf(w.descriptorType)) {
        case Kind::Buffer:
            buffer_infos += w.descriptorCount;
            paired += w.descriptorCount;
            break;
        case Kind::Image:
            image_infos += w.descriptorCount;
            paired += w.descriptorCount;
            break;
        default:
            break;
        }
    }
    if (size_t{base} + paired * sizeof(u64) + 8 > bytes) {
        return false;
    }
    const std::byte* handles = src + base;
    u32 table[2];
    std::memcpy(table, handles + paired * sizeof(u64), sizeof(table));
    const std::byte* cameras = handles + paired * sizeof(u64) + sizeof(table);
    if (size_t{base} + paired * sizeof(u64) + 8 + size_t{table[0]} * sizeof(Camera) > bytes) {
        return false;
    }
    out.buffers.resize(2 * buffer_infos);
    out.images.resize(2 * image_infos);
    size_t ob = 0, oi = 0, at = 0, buffer_index = 0, camera = 0;
    for (auto& w : out.writes) {
        const u32 n = w.descriptorCount;
        switch (KindOf(w.descriptorType)) {
        case Kind::Buffer: {
            vk::DescriptorBufferInfo* const first = out.buffers.data() + ob;
            for (u32 i = 0; i < n; ++i, ++at, ++buffer_index) {
                const vk::DescriptorBufferInfo left = w.pBufferInfo[i];
                vk::DescriptorBufferInfo right = left;
                u64 handle;
                std::memcpy(&handle, handles + at * sizeof(u64), sizeof(handle));
                right.buffer = vk::Buffer{reinterpret_cast<VkBuffer>(handle)};
                if (camera < table[0]) {
                    Camera c;
                    std::memcpy(&c, cameras + camera * sizeof(Camera), sizeof(c));
                    if (c.index == buffer_index) {
                        right = c.right;
                        ++camera;
                    }
                }
                first[2 * i] = left;
                first[2 * i + 1] = right;
            }
            ob += 2 * size_t{n};
            w.descriptorCount = 2 * n;
            w.pBufferInfo = first;
            break;
        }
        case Kind::Image: {
            vk::DescriptorImageInfo* const first = out.images.data() + oi;
            for (u32 i = 0; i < n; ++i, ++at) {
                const vk::DescriptorImageInfo left = w.pImageInfo[i];
                vk::DescriptorImageInfo right = left;
                u64 handle;
                std::memcpy(&handle, handles + at * sizeof(u64), sizeof(handle));
                right.imageView = vk::ImageView{reinterpret_cast<VkImageView>(handle)};
                first[2 * i] = left;
                first[2 * i + 1] = right;
            }
            oi += 2 * size_t{n};
            w.descriptorCount = 2 * n;
            w.pImageInfo = first;
            break;
        }
        default:
            break;
        }
    }
    return camera == table[0];
}

/// VR_COMPACT_SETS=v: what the set receives, field by field (each write's binding, element,
/// count and type; each info's handles, offsets, ranges and layouts), so two writes that set
/// the same descriptors hash alike whatever their padding bytes hold.
inline u64 FieldHash(std::span<const vk::WriteDescriptorSet> writes) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](u64 v) { hash = (hash ^ v) * 0x100000001b3ull; };
    for (const auto& w : writes) {
        mix(w.dstBinding);
        mix(w.dstArrayElement);
        mix(w.descriptorCount);
        mix(static_cast<u64>(w.descriptorType));
        for (u32 i = 0; i < w.descriptorCount; ++i) {
            switch (w.descriptorType) {
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eStorageBuffer:
            case vk::DescriptorType::eUniformBufferDynamic:
            case vk::DescriptorType::eStorageBufferDynamic: {
                const auto& b = w.pBufferInfo[i];
                mix(reinterpret_cast<u64>(static_cast<VkBuffer>(b.buffer)));
                mix(b.offset);
                mix(b.range);
                break;
            }
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                mix(reinterpret_cast<u64>(static_cast<VkBufferView>(w.pTexelBufferView[i])));
                break;
            default: {
                const auto& im = w.pImageInfo[i];
                mix(reinterpret_cast<u64>(static_cast<VkSampler>(im.sampler)));
                mix(reinterpret_cast<u64>(static_cast<VkImageView>(im.imageView)));
                mix(static_cast<u64>(im.imageLayout));
                break;
            }
            }
        }
    }
    return hash;
}

} // namespace Vulkan::StereoSetCodec
