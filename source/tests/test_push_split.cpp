// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_PUSH_SPLIT's held block (src/video_core/renderer_vulkan/vk_push_split.h, used by
// CommandRecorder::PushSplit): random streams of native draws (a head that changes often, a VR
// tail that changes rarely, now and then another shape), mixed with another push, a raw borrow,
// a control packet that records, a guest signal (which records nothing) and new command buffers,
// replay into a mock command buffer. At every draw it must hold exactly the block a whole push
// (FAST_CPU_PUSH_SPLIT=0) leaves, and a stream that forgot to forget must be caught.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_push_split.h"

namespace {

using Vulkan::HeldPush;
using Encode = HeldPush::Encode;

constexpr std::uint32_t Split = 128; // Shader::VrPushData::Begin
constexpr std::uint32_t Size = 224;  // Shader::VrPushData::End
constexpr std::uint32_t Graphics = 0x1f;

// The push constant bytes a command buffer holds as replay leaves them.
struct MockCommandBuffer {
    std::array<std::byte, HeldPush::MaxBytes> bytes{};
    void Push(std::uint32_t offset, std::uint32_t size, const std::byte* values) {
        std::memcpy(bytes.data() + offset, values, size);
    }
    void Scramble(std::mt19937& rng) { // a new command buffer: undefined until pushed
        for (auto& b : bytes) {
            b = std::byte(rng());
        }
    }
};

struct Stats {
    int draws = 0, wrong = 0, heads = 0, nothing = 0;
};

// One random stream. `forget_raw`: a raw borrow forgets the held block, as Raw() does.
Stats Run(std::uint32_t seed, bool forget_raw) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> event(0, 99);
    MockCommandBuffer command;
    command.Scramble(rng);
    HeldPush held;
    std::array<std::byte, HeldPush::MaxBytes> block{};
    for (auto& b : block) {
        b = std::byte(rng());
    }
    std::uint32_t stages = Graphics, split = Split;
    Stats stats;
    for (int i = 0; i < 200000; ++i) {
        const int e = event(rng);
        if (e < 2) { // another push (RecorderProxy::pushConstants): forgets
            std::array<std::byte, 16> other{};
            for (auto& b : other) {
                b = std::byte(rng());
            }
            command.Push(rng() % (Size - 16), 16, other.data());
            held.Forget();
            continue;
        }
        if (e < 3) { // a raw borrow may record anything
            command.Push(Split + rng() % (Size - Split - 1), 1, &block[rng() % Size]);
            command.bytes[Split + rng() % (Size - Split)] ^= std::byte{0x5a};
            if (forget_raw) {
                held.Forget();
            }
            continue;
        }
        if (e < 4) { // a control packet that records (a submit, a frame marker)
            held.Forget();
            continue;
        }
        if (e < 8) { // a guest signal: its replay records nothing, the block stays held
            continue;
        }
        if (e < 9) { // a new command buffer (Configure)
            command.Scramble(rng);
            held.Forget();
            continue;
        }
        // A draw: the head changes most of the time, the tail now and then, the shape rarely.
        if (event(rng) < 70) {
            block[rng() % Split] = std::byte(rng());
        }
        if (event(rng) < 5) {
            block[Split + rng() % (Size - Split)] = std::byte(rng());
        }
        if (event(rng) < 1) {
            stages = stages == Graphics ? 0x11 : Graphics;
        }
        if (event(rng) < 1) {
            split = split == Split ? Split + 16 : Split;
        }
        const Encode encode = held.Next(stages, split, Size, block.data());
        if (encode == Encode::Whole) {
            command.Push(0, Size, block.data());
        } else if (encode == Encode::Head) {
            command.Push(0, split, block.data());
            ++stats.heads;
        } else {
            ++stats.nothing;
        }
        ++stats.draws;
        stats.wrong += std::memcmp(command.bytes.data(), block.data(), Size) != 0;
    }
    return stats;
}

TEST(PushSplit, EveryDrawSeesTheWholePushsBytes) {
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        const Stats stats = Run(seed, true);
        EXPECT_EQ(stats.wrong, 0);
        EXPECT_GT(stats.heads, stats.draws / 4); // most draws push the head alone
        EXPECT_GT(stats.nothing, 0);             // and some nothing at all
    }
}

TEST(PushSplit, AStreamThatDoesNotForgetIsCaught) {
    // The same streams with a raw borrow that does not forget: some draw sees stale bytes.
    int wrong = 0;
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        wrong += Run(seed, false).wrong;
    }
    EXPECT_GT(wrong, 0);
}

TEST(PushSplit, PeekChangesNothingAndNextHoldsTheBlock) {
    HeldPush held;
    std::array<std::byte, Size> a{}, b{};
    b[0] = std::byte{1}; // another head
    EXPECT_TRUE(held.Peek(Graphics, Split, Size, a.data()) == Encode::Whole);
    EXPECT_FALSE(held.valid);
    EXPECT_TRUE(held.Next(Graphics, Split, Size, a.data()) == Encode::Whole);
    EXPECT_TRUE(held.Peek(Graphics, Split, Size, a.data()) == Encode::None);
    EXPECT_TRUE(held.Peek(Graphics, Split, Size, b.data()) == Encode::Head);
    EXPECT_TRUE(held.Next(Graphics, Split, Size, b.data()) == Encode::Head);
    EXPECT_TRUE(held.Next(Graphics, Split, Size, b.data()) == Encode::None);
    b[Size - 1] = std::byte{2}; // another tail
    EXPECT_TRUE(held.Next(Graphics, Split, Size, b.data()) == Encode::Whole);
    EXPECT_TRUE(held.Peek(0x11, Split, Size, b.data()) == Encode::Whole);         // other stages
    EXPECT_TRUE(held.Peek(Graphics, Split + 8, Size, b.data()) == Encode::Whole); // other split
    held.Forget();
    EXPECT_TRUE(held.Peek(Graphics, Split, Size, b.data()) == Encode::Whole);
}

TEST(PushSplit, OnlyABlockWithASplitInsideFits) {
    EXPECT_TRUE(HeldPush::Fits(Split, Size));
    EXPECT_FALSE(HeldPush::Fits(0, Size));
    EXPECT_FALSE(HeldPush::Fits(Size, Size));
    EXPECT_FALSE(HeldPush::Fits(Split, HeldPush::MaxBytes + 4));
}

} // namespace
