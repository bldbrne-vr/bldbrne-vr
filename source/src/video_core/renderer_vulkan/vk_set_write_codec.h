// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::SetWriteCodec {

/// FAST_CPU_DRIVER_CALLS (RecorderProxy::writeDescriptorSet): the descriptor writes of a layout
/// without push descriptors, carried in a recorder packet so the replay commits the set and writes
/// it (vkAllocateDescriptorSets and vkUpdateDescriptorSets were 5.8% of Stage B, 2026-10-14). The
/// encoding is every write's entry (16 bytes instead of the 64 of a vk::WriteDescriptorSet), then
/// every write's infos copied by value in write order; it holds no pointer, so it can sit anywhere
/// in the ring. Decode rebuilds the writes with their info pointers into the encoding.
struct Entry {
    u32 binding;
    u32 element;
    u32 count;
    vk::DescriptorType type;
};
static_assert(sizeof(Entry) == 16);
static_assert(sizeof(vk::DescriptorBufferInfo) % 8 == 0 && sizeof(vk::DescriptorImageInfo) % 8 == 0 &&
              sizeof(vk::BufferView) % 8 == 0); // every write's infos keep the next one aligned

/// Bytes of the infos `w` points to; 0 when the encoding cannot carry it (an extension structure,
/// no descriptors, an inline uniform block or another type without plain infos).
inline u32 PayloadBytes(const vk::WriteDescriptorSet& w) noexcept {
    if (w.pNext || w.descriptorCount == 0) {
        return 0;
    }
    switch (w.descriptorType) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return w.pBufferInfo ? w.descriptorCount * u32{sizeof(vk::DescriptorBufferInfo)} : 0;
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return w.pImageInfo ? w.descriptorCount * u32{sizeof(vk::DescriptorImageInfo)} : 0;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return w.pTexelBufferView ? w.descriptorCount * u32{sizeof(vk::BufferView)} : 0;
    default:
        return 0;
    }
}

inline const void* Payload(const vk::WriteDescriptorSet& w) noexcept {
    switch (w.descriptorType) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return w.pBufferInfo;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return w.pTexelBufferView;
    default:
        return w.pImageInfo;
    }
}

/// The encoding's size (a multiple of 8), or 0 when a write cannot be carried (PayloadBytes).
inline u32 EncodedBytes(std::span<const vk::WriteDescriptorSet> writes) noexcept {
    u32 bytes = static_cast<u32>(writes.size() * sizeof(Entry));
    for (const auto& w : writes) {
        const u32 n = PayloadBytes(w);
        if (n == 0) {
            return 0;
        }
        bytes += n;
    }
    return bytes;
}

/// Writes the encoding of `writes` (EncodedBytes non-zero) to `dst`, EncodedBytes long.
inline void Encode(std::byte* dst, std::span<const vk::WriteDescriptorSet> writes) noexcept {
    auto* entry = dst;
    auto* tail = dst + writes.size() * sizeof(Entry);
    for (const auto& w : writes) {
        const Entry e{w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType};
        std::memcpy(entry, &e, sizeof(e));
        entry += sizeof(e);
        const u32 n = PayloadBytes(w);
        std::memcpy(tail, Payload(w), n);
        tail += n;
    }
}

/// The `count` writes encoded at `src` (dstSet null), their infos pointing into `src`.
inline void Decode(const std::byte* src, u32 count, vk::WriteDescriptorSet* out) noexcept {
    const auto* entry = src;
    const auto* tail = src + size_t{count} * sizeof(Entry);
    for (u32 i = 0; i < count; ++i) {
        Entry e;
        std::memcpy(&e, entry, sizeof(e));
        entry += sizeof(e);
        auto& w = out[i];
        w = vk::WriteDescriptorSet{
            .dstBinding = e.binding,
            .dstArrayElement = e.element,
            .descriptorCount = e.count,
            .descriptorType = e.type,
        };
        switch (e.type) {
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
        case vk::DescriptorType::eUniformBufferDynamic:
        case vk::DescriptorType::eStorageBufferDynamic:
            w.pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(tail);
            break;
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            w.pTexelBufferView = reinterpret_cast<const vk::BufferView*>(tail);
            break;
        default:
            w.pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(tail);
            break;
        }
        tail += PayloadBytes(w);
    }
}

/// FAST_CPU_DRIVER_CALLS_VERIFY: FNV-1a over what the set receives from `writes` (each write's
/// binding, element, count, type and info bytes; dstSet and pNext left out). The recording thread
/// hashes the caller's writes, the replay the decoded ones.
inline u64 Hash(std::span<const vk::WriteDescriptorSet> writes) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            hash = (hash ^ p[i]) * 0x100000001b3ull;
        }
    };
    for (const auto& w : writes) {
        const Entry e{w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType};
        mix(&e, sizeof(e));
        if (const u32 n = PayloadBytes(w)) {
            mix(Payload(w), n);
        }
    }
    return hash;
}

} // namespace Vulkan::SetWriteCodec
