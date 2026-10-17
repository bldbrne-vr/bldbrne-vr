// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "common/types.h"

namespace Vulkan::PushTemplate {

/// FAST_CPU_PUSH_TEMPLATE (Pipeline::BulkTemplateFor, RecorderProxy::pushDescriptorSetTemplate):
/// a native stereo push that FAST bit 28 encodes as its writes plus two info arrays goes through
/// a descriptor update template of the pipeline instead, and its payload is the two arrays alone:
/// the buffer infos, then the image infos (samplers folded in), every info InfoBytes long. The
/// template has one entry per write, giving the descriptors the write sets and the payload offset
/// of its first info; the writes themselves are not copied. PairDescriptors lays the infos out in
/// write order, so a pipeline's writes keep one shape from draw to draw; every push is checked
/// against the template's shape (Matches) and any other takes the bulk packet.
/// Templated over the write and info types so the unit test can use plain structs with
/// vk::WriteDescriptorSet's member names.

/// The size of VkDescriptorBufferInfo and of VkDescriptorImageInfo (the callers assert it).
inline constexpr u32 InfoBytes = 24;

/// The info array a write points into: Other for any a template entry here does not read.
enum class Kind : u8 { Other, Buffer, Image };

/// One template entry: the write's binding, first array element, descriptor count and type, and
/// the payload byte offset of its first info (the next ones follow every InfoBytes).
struct Slot {
    u32 binding, element, count, type;
    u64 offset;
    bool operator==(const Slot&) const = default;
};

inline constexpr u64 NotWithin = ~u64{0};

/// Byte offset of info[0, count) inside `array`; NotWithin for null, empty or outside ranges
/// (the recorder's bulk encoding tests a write's infos the same way).
template <class T>
u64 OffsetWithin(const T* info, u32 count, std::span<const T> array) noexcept {
    const u64 at = static_cast<u64>(reinterpret_cast<std::uintptr_t>(info) -
                                    reinterpret_cast<std::uintptr_t>(array.data()));
    const u64 bytes = u64{count} * sizeof(T);
    return info != nullptr && count != 0 && bytes <= array.size_bytes() &&
                   at <= array.size_bytes() - bytes
               ? at
               : NotWithin;
}

/// The write's slot; its offset is NotWithin when no template entry can take the write (an
/// extension chain, another kind of info, infos outside the two arrays).
template <class Write, class BufferInfo, class ImageInfo, class KindOf>
Slot SlotOf(const Write& w, std::span<const BufferInfo> buffers, std::span<const ImageInfo> images,
            const KindOf& kind_of) noexcept {
    Slot slot{w.dstBinding, w.dstArrayElement, w.descriptorCount,
              static_cast<u32>(w.descriptorType), NotWithin};
    if (w.pNext != nullptr) {
        return slot;
    }
    switch (kind_of(w.descriptorType)) {
    case Kind::Buffer:
        slot.offset = OffsetWithin(w.pBufferInfo, w.descriptorCount, buffers);
        break;
    case Kind::Image: {
        const u64 at = OffsetWithin(w.pImageInfo, w.descriptorCount, images);
        slot.offset = at == NotWithin ? NotWithin : buffers.size_bytes() + at;
        break;
    }
    case Kind::Other:
        break;
    }
    return slot;
}

/// The writes' slots into `out` (as many as writes); false when a write takes none.
template <class Write, class BufferInfo, class ImageInfo, class KindOf>
bool MakeSlots(std::span<const Write> writes, std::span<const BufferInfo> buffers,
               std::span<const ImageInfo> images, const KindOf& kind_of,
               std::span<Slot> out) noexcept {
    if (writes.empty() || out.size() != writes.size()) {
        return false;
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        out[i] = SlotOf(writes[i], buffers, images, kind_of);
        if (out[i].offset == NotWithin) {
            return false;
        }
    }
    return true;
}

/// True when the writes have exactly these slots: the template made from the slots then sets
/// the same descriptors from this push's payload as the writes do.
template <class Write, class BufferInfo, class ImageInfo, class KindOf>
bool Matches(std::span<const Write> writes, std::span<const BufferInfo> buffers,
             std::span<const ImageInfo> images, const KindOf& kind_of,
             std::span<const Slot> slots) noexcept {
    if (writes.size() != slots.size()) {
        return false;
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        const Slot slot = SlotOf(writes[i], buffers, images, kind_of);
        if (slot.offset == NotWithin || !(slot == slots[i])) {
            return false;
        }
    }
    return true;
}

/// The payload's size and bytes: the buffer infos, then the image infos.
template <class BufferInfo, class ImageInfo>
u64 PayloadBytes(std::span<const BufferInfo> buffers, std::span<const ImageInfo> images) noexcept {
    return buffers.size_bytes() + images.size_bytes();
}
template <class BufferInfo, class ImageInfo>
void WritePayload(std::byte* dst, std::span<const BufferInfo> buffers,
                  std::span<const ImageInfo> images) noexcept {
    if (!buffers.empty()) {
        std::memcpy(dst, buffers.data(), buffers.size_bytes());
    }
    if (!images.empty()) {
        std::memcpy(dst + buffers.size_bytes(), images.data(), images.size_bytes());
    }
}

/// FAST_CPU_PUSH_TEMPLATE_VERIFY: reads `payload` as the template's entries do (descriptor j of a
/// slot at its offset + j * InfoBytes) and compares each with the info its write points at:
/// `same(write, j, bytes)` for the InfoBytes there. The slots' binding, element, count and type
/// must be the writes'. Returns the first write that differs, or writes.size() when none does.
template <class Write, class Same>
size_t FirstDiffering(std::span<const Write> writes, std::span<const Slot> slots,
                      std::span<const std::byte> payload, const Same& same) {
    if (writes.size() != slots.size()) {
        return 0;
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        const Write& w = writes[i];
        const Slot& slot = slots[i];
        if (slot.binding != w.dstBinding || slot.element != w.dstArrayElement ||
            slot.count != w.descriptorCount || slot.type != static_cast<u32>(w.descriptorType) ||
            slot.offset > payload.size() ||
            u64{slot.count} * InfoBytes > payload.size() - slot.offset) {
            return i;
        }
        for (u32 j = 0; j < slot.count; ++j) {
            if (!same(w, j, payload.data() + slot.offset + u64{j} * InfoBytes)) {
                return i;
            }
        }
    }
    return writes.size();
}

} // namespace Vulkan::PushTemplate
