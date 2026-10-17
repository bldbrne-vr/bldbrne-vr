// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_DRIVER_CALLS' set-write encoding (src/video_core/renderer_vulkan/vk_set_write_codec.h,
// used by CommandRecorder::WriteDescriptorSet and its replay): random descriptor writes of every
// carried type (buffers, images, samplers, texel views; arrays at any element; info padding bytes
// left random), encoded at a random 8-byte position of a ring-sized buffer and decoded from there,
// must give a set exactly what the caller's writes give it (vkUpdateDescriptorSets modelled on a
// mock set), with the same verify hash; the size must be what the packet reserves; a write the
// encoding cannot carry must make the whole call uncarried, and the hash must see a changed
// descriptor.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_set_write_codec.h"

namespace {

namespace Codec = Vulkan::SetWriteCodec;

constexpr std::array Types{
    vk::DescriptorType::eUniformBuffer,        vk::DescriptorType::eStorageBuffer,
    vk::DescriptorType::eUniformBufferDynamic, vk::DescriptorType::eStorageBufferDynamic,
    vk::DescriptorType::eSampler,              vk::DescriptorType::eCombinedImageSampler,
    vk::DescriptorType::eSampledImage,         vk::DescriptorType::eStorageImage,
    vk::DescriptorType::eInputAttachment,      vk::DescriptorType::eUniformTexelBuffer,
    vk::DescriptorType::eStorageTexelBuffer,
};

enum class Kind { Buffer, Image, Texel };
Kind KindOf(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return Kind::Buffer;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return Kind::Texel;
    default:
        return Kind::Image;
    }
}
std::size_t InfoSize(Kind kind) {
    return kind == Kind::Buffer  ? sizeof(vk::DescriptorBufferInfo)
           : kind == Kind::Image ? sizeof(vk::DescriptorImageInfo)
                                 : sizeof(vk::BufferView);
}

// The caller's writes with their info arrays (as PairDescriptors leaves them: several writes
// pointing into shared arrays, some into arrays of their own).
struct Call {
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<std::vector<std::byte>> infos; // raw bytes, padding included
};

Call RandomCall(std::mt19937& rng, std::size_t count) {
    Call call;
    call.writes.reserve(count);
    call.infos.reserve(count);
    std::uniform_int_distribution<std::size_t> type_of(0, Types.size() - 1);
    std::uniform_int_distribution<std::uint32_t> count_of(1, 4), binding_of(0, 63), element_of(0, 3);
    for (std::size_t i = 0; i < count; ++i) {
        const auto type = Types[type_of(rng)];
        const std::uint32_t n = count_of(rng);
        auto& bytes = call.infos.emplace_back(n * InfoSize(KindOf(type)));
        for (auto& b : bytes) {
            b = std::byte(rng()); // handles, offsets, layouts and padding: all arbitrary
        }
        vk::WriteDescriptorSet w{
            .dstSet = std::bit_cast<vk::DescriptorSet>(std::uint64_t{0x1234'0000} + i),
            .dstBinding = binding_of(rng),
            .dstArrayElement = element_of(rng),
            .descriptorCount = n,
            .descriptorType = type,
        };
        switch (KindOf(type)) {
        case Kind::Buffer:
            w.pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(bytes.data());
            break;
        case Kind::Image:
            w.pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(bytes.data());
            break;
        case Kind::Texel:
            w.pTexelBufferView = reinterpret_cast<const vk::BufferView*>(bytes.data());
            break;
        }
        call.writes.push_back(w);
    }
    return call;
}

// What vkUpdateDescriptorSets leaves in a set: per (binding, array element) the descriptor's type
// and info bytes; later writes replace earlier ones.
using MockSet = std::map<std::pair<std::uint32_t, std::uint32_t>,
                         std::pair<vk::DescriptorType, std::vector<std::byte>>>;
MockSet Update(std::span<const vk::WriteDescriptorSet> writes) {
    MockSet set;
    for (const auto& w : writes) {
        const std::size_t size = InfoSize(KindOf(w.descriptorType));
        const auto* info = static_cast<const std::byte*>(Codec::Payload(w));
        for (std::uint32_t i = 0; i < w.descriptorCount; ++i) {
            set[{w.dstBinding, w.dstArrayElement + i}] = {
                w.descriptorType, std::vector<std::byte>(info + i * size, info + (i + 1) * size)};
        }
    }
    return set;
}

std::size_t ExpectedBytes(const Call& call) {
    std::size_t bytes = call.writes.size() * sizeof(Codec::Entry);
    for (const auto& info : call.infos) {
        bytes += info.size();
    }
    return bytes;
}

TEST(SetWriteCodec, RoundTripGivesTheSetTheSameDescriptors) {
    std::mt19937 rng(20261014);
    // A ring stand-in: the encoding lands at arbitrary 8-byte positions, as packets do.
    std::vector<std::uint64_t> ring(1 << 16);
    auto* base = reinterpret_cast<std::byte*>(ring.data());
    std::uniform_int_distribution<std::size_t> count_of(1, 40);
    for (int round = 0; round < 4000; ++round) {
        const Call call = RandomCall(rng, count_of(rng));
        const std::span<const vk::WriteDescriptorSet> writes{call.writes};
        const std::uint32_t bytes = Codec::EncodedBytes(writes);
        ASSERT_EQ(bytes, ExpectedBytes(call));
        ASSERT_EQ(bytes % 8, 0u);
        const std::size_t slots = ring.size() - bytes / 8;
        std::byte* at = base + 8 * std::uniform_int_distribution<std::size_t>(0, slots - 1)(rng);
        std::memset(at, 0xcd, bytes);
        Codec::Encode(at, writes);

        std::vector<vk::WriteDescriptorSet> decoded(writes.size());
        Codec::Decode(at, static_cast<std::uint32_t>(writes.size()), decoded.data());
        for (std::size_t i = 0; i < writes.size(); ++i) {
            const auto& w = writes[i];
            const auto& d = decoded[i];
            ASSERT_TRUE(d.sType == vk::StructureType::eWriteDescriptorSet);
            ASSERT_EQ(d.pNext, nullptr);
            ASSERT_TRUE(d.dstSet == vk::DescriptorSet{}); // the replay's commit sets it
            ASSERT_EQ(d.dstBinding, w.dstBinding);
            ASSERT_EQ(d.dstArrayElement, w.dstArrayElement);
            ASSERT_EQ(d.descriptorCount, w.descriptorCount);
            ASSERT_TRUE(d.descriptorType == w.descriptorType);
            const Kind kind = KindOf(w.descriptorType);
            ASSERT_EQ(d.pBufferInfo != nullptr, kind == Kind::Buffer);
            ASSERT_EQ(d.pImageInfo != nullptr, kind == Kind::Image);
            ASSERT_EQ(d.pTexelBufferView != nullptr, kind == Kind::Texel);
            const auto* info = static_cast<const std::byte*>(Codec::Payload(d));
            ASSERT_TRUE(info >= at && info + call.infos[i].size() <= at + bytes);
            ASSERT_EQ(std::memcmp(info, call.infos[i].data(), call.infos[i].size()), 0);
        }
        ASSERT_TRUE(Update(decoded) == Update(writes));
        ASSERT_EQ(Codec::Hash(decoded), Codec::Hash(writes));
    }
}

TEST(SetWriteCodec, UncarriedWriteLeavesTheCallUncarried) {
    std::mt19937 rng(7);
    Call call = RandomCall(rng, 12);
    ASSERT_NE(Codec::EncodedBytes(call.writes), 0u);
    const vk::PhysicalDeviceFeatures2 extension{}; // any structure chained to a write
    const auto uncarried = [&](auto&& change) {
        auto writes = call.writes;
        change(writes[5]);
        return Codec::EncodedBytes(writes) == 0;
    };
    EXPECT_TRUE(uncarried([&](vk::WriteDescriptorSet& w) { w.pNext = &extension; }));
    EXPECT_TRUE(uncarried([](vk::WriteDescriptorSet& w) { w.descriptorCount = 0; }));
    EXPECT_TRUE(uncarried([](vk::WriteDescriptorSet& w) {
        w.descriptorType = vk::DescriptorType::eInlineUniformBlock;
    }));
    EXPECT_TRUE(uncarried([](vk::WriteDescriptorSet& w) {
        w.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    }));
    EXPECT_TRUE(uncarried([](vk::WriteDescriptorSet& w) {
        w.pBufferInfo = nullptr;
        w.pImageInfo = nullptr;
        w.pTexelBufferView = nullptr;
    }));
    // No writes: nothing to commit a set for, nothing carried either.
    EXPECT_EQ(Codec::EncodedBytes({}), 0u);
}

TEST(SetWriteCodec, HashSeesEveryChange) {
    std::mt19937 rng(99);
    for (int round = 0; round < 200; ++round) {
        Call call = RandomCall(rng, 16);
        const std::uint64_t hash = Codec::Hash(call.writes);
        // The caller's dstSet is not part of what the set receives.
        auto writes = call.writes;
        writes[3].dstSet = vk::DescriptorSet{};
        ASSERT_EQ(Codec::Hash(writes), hash);
        writes = call.writes;
        ++writes[7].dstBinding;
        ASSERT_NE(Codec::Hash(writes), hash);
        writes = call.writes;
        ++writes[7].dstArrayElement;
        ASSERT_NE(Codec::Hash(writes), hash);
        // One flipped info byte anywhere.
        auto& info = call.infos[std::uniform_int_distribution<std::size_t>(0, 15)(rng)];
        auto& b = info[std::uniform_int_distribution<std::size_t>(0, info.size() - 1)(rng)];
        b ^= std::byte{0x10};
        ASSERT_NE(Codec::Hash(call.writes), hash);
    }
}

} // namespace
