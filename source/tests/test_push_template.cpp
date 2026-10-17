// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_PUSH_TEMPLATE's slots (src/video_core/renderer_vulkan/vk_push_template.h, used by
// Pipeline::BulkTemplateFor and RecorderProxy::pushDescriptorSetTemplate): random native draws
// laid out as PairDescriptors lays them (buffer and image infos in left/right pairs, samplers
// folded into the image infos, all in write order), read back through their template from the
// payload the recorder copies, give every descriptor of every write. A later push of the same
// shape matches the template and reads its own descriptors through it; any other shape, and any
// write no entry can take, does not.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_push_template.h"

namespace {

using namespace Vulkan::PushTemplate;

// vk::DescriptorType's values for the types a native push uses (and a texel one it does not).
enum class Type : std::uint32_t {
    Sampler = 0,
    SampledImage = 2,
    StorageImage = 3,
    UniformTexelBuffer = 4,
    StorageBuffer = 7,
};
struct BufferInfo {
    std::uint64_t buffer, offset, range;
};
struct ImageInfo {
    std::uint64_t sampler, view;
    std::uint32_t layout, padding;
};
static_assert(sizeof(BufferInfo) == InfoBytes && sizeof(ImageInfo) == InfoBytes);
// vk::WriteDescriptorSet's members as the slots read them.
struct Write {
    const void* pNext{};
    std::uint32_t dstBinding{}, dstArrayElement{}, descriptorCount{};
    Type descriptorType{};
    const ImageInfo* pImageInfo{};
    const BufferInfo* pBufferInfo{};
};
struct KindOf {
    Kind operator()(Type type) const noexcept {
        switch (type) {
        case Type::StorageBuffer:
            return Kind::Buffer;
        case Type::Sampler:
        case Type::SampledImage:
        case Type::StorageImage:
            return Kind::Image;
        default:
            return Kind::Other;
        }
    }
};

struct Resource {
    Type type;
    std::uint32_t count; // descriptors before pairing
};
// A paired push: the writes point into the two arrays (never reallocated after laying out).
struct Draw {
    std::vector<Write> writes;
    std::vector<BufferInfo> buffers;
    std::vector<ImageInfo> images;
    std::span<const Write> Writes() const {
        return {writes.data(), writes.size()};
    }
    std::span<const BufferInfo> Buffers() const {
        return {buffers.data(), buffers.size()};
    }
    std::span<const ImageInfo> Images() const {
        return {images.data(), images.size()};
    }
};

// NativeStereo::PairDescriptors' output for these resources (one write each, bindings in order).
Draw Lay(const std::vector<Resource>& resources, std::mt19937_64& rng) {
    Draw draw;
    std::size_t capacity = 0;
    for (const auto& r : resources) {
        capacity += 2 * r.count;
    }
    draw.buffers.reserve(capacity);
    draw.images.reserve(capacity);
    std::uint32_t binding = 0;
    for (const auto& r : resources) {
        Write w{.dstBinding = binding, .descriptorCount = r.count, .descriptorType = r.type};
        binding += r.type == Type::StorageBuffer || r.type == Type::Sampler ? 1 : r.count;
        if (r.type == Type::StorageBuffer) {
            const std::size_t first = draw.buffers.size();
            for (std::uint32_t i = 0; i < 2 * r.count; ++i) {
                draw.buffers.push_back({rng(), rng(), rng()});
            }
            w.descriptorCount *= 2;
            w.pBufferInfo = draw.buffers.data() + first;
        } else {
            const std::size_t first = draw.images.size();
            const std::uint32_t infos = r.type == Type::Sampler ? r.count : 2 * r.count;
            for (std::uint32_t i = 0; i < infos; ++i) {
                draw.images.push_back({rng(), rng(), static_cast<std::uint32_t>(rng()),
                                       static_cast<std::uint32_t>(rng())});
            }
            w.descriptorCount = infos;
            w.pImageInfo = draw.images.data() + first;
        }
        draw.writes.push_back(w);
    }
    return draw;
}

std::vector<Resource> RandomResources(std::mt19937_64& rng) {
    std::vector<Resource> resources;
    const int count = 1 + static_cast<int>(rng() % 12);
    for (int i = 0; i < count; ++i) {
        static constexpr Type types[] = {Type::StorageBuffer, Type::SampledImage,
                                         Type::StorageImage, Type::Sampler};
        const Type type = types[rng() % 4];
        const std::uint32_t n =
            type == Type::SampledImage ? 1 + static_cast<std::uint32_t>(rng() % 3) : 1u;
        resources.push_back({type, n});
    }
    return resources;
}

std::vector<std::byte> Payload(const Draw& draw) {
    std::vector<std::byte> payload(PayloadBytes(draw.Buffers(), draw.Images()));
    WritePayload(payload.data(), draw.Buffers(), draw.Images());
    return payload;
}

// The descriptor the template reads (`bytes`) is the one the write points at.
bool SameDescriptor(const Write& w, std::uint32_t j, const std::byte* bytes) {
    if (KindOf{}(w.descriptorType) == Kind::Buffer) {
        BufferInfo info;
        std::memcpy(&info, bytes, sizeof(info));
        return info.buffer == w.pBufferInfo[j].buffer && info.offset == w.pBufferInfo[j].offset &&
               info.range == w.pBufferInfo[j].range;
    }
    ImageInfo info;
    std::memcpy(&info, bytes, sizeof(info));
    return info.sampler == w.pImageInfo[j].sampler && info.view == w.pImageInfo[j].view &&
           info.layout == w.pImageInfo[j].layout;
}

std::vector<Slot> SlotsOf(const Draw& draw) {
    std::vector<Slot> slots(draw.writes.size());
    EXPECT_TRUE(
        MakeSlots(draw.Writes(), draw.Buffers(), draw.Images(), KindOf{}, std::span<Slot>{slots}));
    return slots;
}

TEST(PushTemplate, RandomDrawsReadBackThroughTheirTemplate) {
    std::mt19937_64 rng(0x5eed);
    for (int round = 0; round < 4000; ++round) {
        const Draw draw = Lay(RandomResources(rng), rng);
        const std::vector<Slot> slots = SlotsOf(draw);
        const std::vector<std::byte> payload = Payload(draw);
        ASSERT_EQ(FirstDiffering(draw.Writes(), std::span<const Slot>{slots},
                                 std::span<const std::byte>{payload}, &SameDescriptor),
                  draw.writes.size());
        ASSERT_TRUE(Matches(draw.Writes(), draw.Buffers(), draw.Images(), KindOf{},
                            std::span<const Slot>{slots}));
        // The image infos follow every buffer info, in write order; the infos are contiguous.
        u64 next_buffer = 0, next_image = draw.buffers.size() * InfoBytes;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            u64& next = draw.writes[i].pBufferInfo ? next_buffer : next_image;
            ASSERT_EQ(slots[i].offset, next);
            next += u64{slots[i].count} * InfoBytes;
        }
        ASSERT_EQ(next_image, payload.size());
    }
}

TEST(PushTemplate, SameShapeReadsItsOwnDescriptors) {
    std::mt19937_64 rng(0xa11);
    for (int round = 0; round < 1000; ++round) {
        const std::vector<Resource> resources = RandomResources(rng);
        const Draw first = Lay(resources, rng);
        const std::vector<Slot> slots = SlotsOf(first);
        const Draw later = Lay(resources, rng); // other handles, same shape
        ASSERT_TRUE(Matches(later.Writes(), later.Buffers(), later.Images(), KindOf{},
                            std::span<const Slot>{slots}));
        const std::vector<std::byte> payload = Payload(later);
        ASSERT_EQ(FirstDiffering(later.Writes(), std::span<const Slot>{slots},
                                 std::span<const std::byte>{payload}, &SameDescriptor),
                  later.writes.size());
        // The first push's payload is not the later one's descriptors.
        const std::vector<std::byte> stale = Payload(first);
        ASSERT_NE(FirstDiffering(later.Writes(), std::span<const Slot>{slots},
                                 std::span<const std::byte>{stale}, &SameDescriptor),
                  later.writes.size());
    }
}

TEST(PushTemplate, OtherShapesDoNotMatch) {
    std::mt19937_64 rng(0xbeef);
    for (int round = 0; round < 1000; ++round) {
        std::vector<Resource> resources = RandomResources(rng);
        const Draw first = Lay(resources, rng);
        const std::vector<Slot> slots = SlotsOf(first);
        const std::span<const Slot> template_slots{slots};
        std::vector<Resource> other = resources;
        const std::size_t at = rng() % other.size();
        switch (rng() % 4) {
        case 0: // one more descriptor
            other[at].count += other[at].type == Type::SampledImage ? 1 : 0;
            if (other[at].type != Type::SampledImage) {
                other.push_back({Type::Sampler, 1});
            }
            break;
        case 1: // another type
            other[at].type =
                other[at].type == Type::StorageBuffer ? Type::StorageImage : Type::StorageBuffer;
            break;
        case 2: // a resource fewer
            if (other.size() == 1) {
                other.push_back({Type::Sampler, 1});
            } else {
                other.erase(other.begin() + static_cast<std::ptrdiff_t>(at));
            }
            break;
        default: // a resource more
            other.insert(other.begin() + static_cast<std::ptrdiff_t>(at), {Type::StorageBuffer, 1});
            break;
        }
        const Draw changed = Lay(other, rng);
        ASSERT_FALSE(Matches(changed.Writes(), changed.Buffers(), changed.Images(), KindOf{},
                             template_slots));
    }
    // The same writes with another binding or first array element.
    const Draw draw = Lay({{Type::StorageBuffer, 1}, {Type::SampledImage, 2}}, rng);
    const std::vector<Slot> slots = SlotsOf(draw);
    for (int field = 0; field < 2; ++field) {
        Draw moved = Lay({{Type::StorageBuffer, 1}, {Type::SampledImage, 2}}, rng);
        (field == 0 ? moved.writes[1].dstBinding : moved.writes[1].dstArrayElement) += 1;
        ASSERT_FALSE(Matches(moved.Writes(), moved.Buffers(), moved.Images(), KindOf{},
                             std::span<const Slot>{slots}));
    }
    // A buffer info more at the front moves every image offset: the infos no longer line up.
    Draw shifted = Lay({{Type::StorageBuffer, 1}, {Type::SampledImage, 2}}, rng);
    shifted.buffers.insert(shifted.buffers.begin(), BufferInfo{});
    shifted.writes[0].pBufferInfo = shifted.buffers.data() + 1;
    ASSERT_FALSE(Matches(shifted.Writes(), shifted.Buffers(), shifted.Images(), KindOf{},
                         std::span<const Slot>{slots}));
}

TEST(PushTemplate, WritesNoEntryTakes) {
    std::mt19937_64 rng(7);
    const std::vector<Resource> resources{
        {Type::StorageBuffer, 1}, {Type::SampledImage, 1}, {Type::Sampler, 1}};
    const auto rejected = [](const Draw& draw) {
        std::vector<Slot> slots(draw.writes.size());
        return !MakeSlots(draw.Writes(), draw.Buffers(), draw.Images(), KindOf{},
                          std::span<Slot>{slots});
    };
    {
        Draw draw = Lay(resources, rng);
        int chain = 0;
        draw.writes[1].pNext = &chain; // an extension chain
        EXPECT_TRUE(rejected(draw));
    }
    {
        Draw draw = Lay(resources, rng);
        draw.writes[1].descriptorType = Type::UniformTexelBuffer; // no info array here
        EXPECT_TRUE(rejected(draw));
    }
    {
        Draw draw = Lay(resources, rng);
        const std::vector<ImageInfo> elsewhere(2);
        draw.writes[1].pImageInfo = elsewhere.data(); // infos outside the two arrays
        EXPECT_TRUE(rejected(draw));
    }
    {
        Draw draw = Lay(resources, rng);
        draw.writes[0].descriptorCount = 3; // runs past the buffer infos
        EXPECT_TRUE(rejected(draw));
    }
    {
        Draw draw = Lay(resources, rng);
        draw.writes[2].descriptorCount = 0;
        EXPECT_TRUE(rejected(draw));
    }
    {
        Draw draw = Lay(resources, rng);
        draw.writes[1].pImageInfo = nullptr;
        EXPECT_TRUE(rejected(draw));
    }
    const Draw empty{};
    EXPECT_TRUE(rejected(empty));
}

TEST(PushTemplate, PayloadTooShortForASlot) {
    std::mt19937_64 rng(11);
    const Draw draw = Lay({{Type::StorageBuffer, 1}, {Type::SampledImage, 2}}, rng);
    const std::vector<Slot> slots = SlotsOf(draw);
    std::vector<std::byte> payload = Payload(draw);
    payload.resize(payload.size() - InfoBytes); // the last image info missing
    EXPECT_EQ(FirstDiffering(draw.Writes(), std::span<const Slot>{slots},
                             std::span<const std::byte>{payload}, &SameDescriptor),
              1u);
}

} // namespace
