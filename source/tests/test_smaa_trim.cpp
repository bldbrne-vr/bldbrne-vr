// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_GPU_SMAA_TRIM (renderer_vulkan/vr_smaa_trim.h): the switch values and the texel compare
// FAST_GPU_SMAA_TRIM_VERIFY runs on the two readback copies of A.

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_smaa_trim.h"

namespace {

using namespace Vulkan::SmaaTrim;

TEST(SmaaTrim, MaskParsing) {
    EXPECT_EQ(ParseMask(nullptr), 0u);
    EXPECT_EQ(ParseMask(""), 0u);
    EXPECT_EQ(ParseMask("0"), 0u);
    EXPECT_EQ(ParseMask("1"), Keep);
    EXPECT_EQ(ParseMask("2"), Compact);
    EXPECT_EQ(ParseMask("3"), Keep | Compact);
    EXPECT_EQ(ParseMask("0x3"), Keep | Compact);
    // Unknown bits are ignored, not taken as a trim.
    EXPECT_EQ(ParseMask("4"), 0u);
    EXPECT_EQ(ParseMask("0xff"), Keep | Compact);
    EXPECT_EQ(ParseMask("off"), 0u);
}

TEST(SmaaTrim, VerifyEveryParsing) {
    EXPECT_EQ(ParseEvery(nullptr), 0u);
    EXPECT_EQ(ParseEvery(""), 0u);
    EXPECT_EQ(ParseEvery("0"), 0u);
    EXPECT_EQ(ParseEvery("1"), 1u);
    EXPECT_EQ(ParseEvery("60"), 60u);
    EXPECT_EQ(ParseEvery("0x10"), 16u);
}

TEST(SmaaTrim, IdenticalCopiesHaveNoDifference) {
    std::vector<u8> a(4 * 64);
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<u8>(i * 7);
    }
    const std::vector<u8> b = a;
    const TexelDiff d = CountTexelDiffs(a.data(), b.data(), 64);
    EXPECT_EQ(d.color, 0u);
    EXPECT_EQ(d.alpha_only, 0u);
    EXPECT_EQ(d.first, ~u64{0});
}

TEST(SmaaTrim, ColourAndAlphaDifferencesAreSplit) {
    std::vector<u8> fast(4 * 8, 0x40);
    std::vector<u8> ref = fast;
    // Texel 2: alpha alone (the keep trim when the game's copy wrote a luma into B's alpha).
    ref[2 * 4 + 3] = 0x99;
    // Texel 5: the colour (first byte) and alpha.
    ref[5 * 4 + 0] = 0x41;
    ref[5 * 4 + 3] = 0x41;
    // Texel 7: the third byte (blue in RGBA, red in BGRA) alone.
    fast[7 * 4 + 2] = 0x00;
    const TexelDiff d = CountTexelDiffs(fast.data(), ref.data(), 8);
    EXPECT_EQ(d.color, 2u);
    EXPECT_EQ(d.alpha_only, 1u);
    EXPECT_EQ(d.first, 2u);
    EXPECT_EQ(d.first_fast, (std::array<u8, 4>{0x40, 0x40, 0x40, 0x40}));
    EXPECT_EQ(d.first_ref, (std::array<u8, 4>{0x40, 0x40, 0x40, 0x99}));
}

TEST(SmaaTrim, OnlyTheGivenTexelsAreCompared) {
    std::vector<u8> fast(4 * 4, 0);
    std::vector<u8> ref(4 * 4, 0);
    ref[3 * 4 + 1] = 1; // texel 3, outside the 3 texels compared
    const TexelDiff d = CountTexelDiffs(fast.data(), ref.data(), 3);
    EXPECT_EQ(d.color + d.alpha_only, 0u);
    EXPECT_EQ(CountTexelDiffs(fast.data(), ref.data(), 4).color, 1u);
}

} // namespace
