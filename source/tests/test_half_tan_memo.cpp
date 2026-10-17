// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_TAN_MEMO's memo (src/video_core/renderer_vulkan/vr_half_tan_memo.h, used by
// Rasterizer::DrawRecord / DrawIndirectRecord for the eye shift): over random streams of camera
// vfovs (runs of one value, as draws of one camera update, alternating values, and edge values:
// zeros of both signs, denormals, infinities, NaNs with different payloads) it must return, bit for
// bit, what std::tan(0.5f * vfov) (FAST_CPU_TAN_MEMO=0) gives at every call, and never a stale
// tangent after the vfov changes.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_half_tan_memo.h"

namespace {

using Vulkan::HalfTanMemo;

std::uint32_t Bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

// The expression the draws used before the memo, written out here (not through Compute).
float Direct(float vfov) {
    return std::tan(.5f * vfov);
}

std::vector<float> EdgeValues() {
    return {0.0f,
            -0.0f,
            std::numeric_limits<float>::denorm_min(),
            -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::min(),
            std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN(),
            std::bit_cast<float>(0x7fc00001u), // another NaN payload
            std::bit_cast<float>(0xffc00000u), // negative quiet NaN
            1.9643f,                           // a headset's vertical fov, radians
            std::nextafter(1.9643f, 2.0f),     // its neighbour: one ulp apart
            3.14159265f,                       // tan near its pole
            1.0f};
}

void ExpectSame(HalfTanMemo& memo, float value) {
    // Through a volatile: no constant folding (at compile time, with another tan) on either side.
    volatile float opaque = value;
    const float vfov = opaque;
    const float expected = Direct(vfov);
    const float got = memo(vfov);
    ASSERT_EQ(Bits(got), Bits(expected)) << "vfov bits " << std::hex << Bits(vfov);
    ASSERT_EQ(Bits(HalfTanMemo::Compute(vfov)), Bits(expected));
}

TEST(HalfTanMemo, EdgeValuesInEveryOrder) {
    const auto values = EdgeValues();
    for (const float first : values) {
        for (const float second : values) {
            HalfTanMemo memo;
            ExpectSame(memo, first);
            ExpectSame(memo, first); // the hit
            ExpectSame(memo, second);
            ExpectSame(memo, first); // back: recomputed, not the second's value
            ExpectSame(memo, second);
        }
    }
}

TEST(HalfTanMemo, SignedZerosAndNaNPayloadsAreSeparateKeys) {
    HalfTanMemo memo;
    ExpectSame(memo, 0.0f);
    ExpectSame(memo, -0.0f);
    EXPECT_EQ(memo.bits, Bits(-0.0f));
    ExpectSame(memo, std::numeric_limits<float>::quiet_NaN());
    ExpectSame(memo, std::bit_cast<float>(0x7fc00001u));
    EXPECT_EQ(memo.bits, 0x7fc00001u);
}

TEST(HalfTanMemo, RandomCameraStreams) {
    std::mt19937 rng(20261014);
    std::uniform_real_distribution<float> fov(0.5f, 2.6f);
    const auto edges = EdgeValues();
    HalfTanMemo memo;
    float current = fov(rng);
    float previous = current;
    for (int i = 0; i < 400000; ++i) {
        switch (rng() % 16) {
        case 0: // a new camera update
            previous = current;
            current = fov(rng);
            break;
        case 1: // one ulp away
            previous = current;
            current = std::nextafter(current, (rng() & 1) ? 4.0f : 0.0f);
            break;
        case 2: // back to the previous camera (two cameras interleaved)
            std::swap(current, previous);
            break;
        case 3: // an odd value now and then
            previous = current;
            current = edges[rng() % edges.size()];
            break;
        default: // the next draw of the same camera update
            break;
        }
        ExpectSame(memo, current);
    }
}

TEST(HalfTanMemo, FreshMemoComputesFirstValue) {
    // A zeroed memo holds bits 0 but is not valid: vfov +0.0f must still be computed.
    HalfTanMemo memo;
    EXPECT_FALSE(memo.valid);
    ExpectSame(memo, 0.0f);
    EXPECT_TRUE(memo.valid);
    HalfTanMemo other;
    ExpectSame(other, 1.25f);
    EXPECT_EQ(other.bits, Bits(1.25f));
}

} // namespace
