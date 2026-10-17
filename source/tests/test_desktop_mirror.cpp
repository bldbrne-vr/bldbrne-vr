// SPDX-License-Identifier: GPL-2.0-or-later

// The desktop window while the headset shows the game (core/vr/desktop_mirror.h; vr.ini
// desktop_mirror, desktop_mirror_view): which published frames are presented, the queue priority
// hint, the eye's centre crop to the window's shape, and the byte order of the eye blit.

#include <cstdlib>

#include <gtest/gtest.h>

#include "core/vr/desktop_mirror.h"

namespace {

namespace M = VR::DesktopMirror;

TEST(DesktopMirror, EveryFramePresentsAll) {
    for (u64 n = 1; n <= 10; ++n) {
        EXPECT_TRUE(M::Presents(1, n));
    }
}

TEST(DesktopMirror, EverySecondFrameIsHalfTheRate) {
    u32 shown = 0;
    for (u64 n = 1; n <= 120; ++n) {
        shown += M::Presents(2, n) ? 1 : 0;
        // Never two skipped or two shown in a row.
        EXPECT_NE(M::Presents(2, n), M::Presents(2, n + 1));
    }
    EXPECT_EQ(shown, 60u); // 60 game frames/s -> 30 presents/s
}

TEST(DesktopMirror, EveryThirdFrame) {
    u32 shown = 0;
    for (u64 n = 1; n <= 90; ++n) {
        shown += M::Presents(3, n) ? 1 : 0;
    }
    EXPECT_EQ(shown, 30u);
    EXPECT_FALSE(M::Presents(3, 1));
    EXPECT_FALSE(M::Presents(3, 2));
    EXPECT_TRUE(M::Presents(3, 3));
}

TEST(DesktopMirror, OffNeverPresents) {
    for (u64 n = 0; n <= 10; ++n) {
        EXPECT_FALSE(M::Presents(0, n));
        EXPECT_FALSE(M::Presents(-1, n));
    }
}

TEST(DesktopMirror, PriorityHintOnlyOffTodaysWindow) {
    EXPECT_FALSE(M::LowPresentPriority(1, M::View::Game)); // the window of before 2026-10-09
    EXPECT_TRUE(M::LowPresentPriority(1, M::View::Left));
    EXPECT_TRUE(M::LowPresentPriority(1, M::View::Right));
    EXPECT_TRUE(M::LowPresentPriority(2, M::View::Game));
    EXPECT_TRUE(M::LowPresentPriority(2, M::View::Right)); // the default
    EXPECT_TRUE(M::LowPresentPriority(0, M::View::Game));
}

TEST(DesktopMirror, ViewsAndTheirEyes) {
    EXPECT_TRUE(M::ParseView("game") == M::View::Game);
    EXPECT_TRUE(M::ParseView("0") == M::View::Game);
    EXPECT_TRUE(M::ParseView("left") == M::View::Left);
    EXPECT_TRUE(M::ParseView("1") == M::View::Left);
    EXPECT_TRUE(M::ParseView("right") == M::View::Right);
    EXPECT_TRUE(M::ParseView("2") == M::View::Right);
    EXPECT_TRUE(M::ParseView("eye") == M::View::Right); // anything else: the default
    EXPECT_EQ(M::EyeOf(M::View::Left), 0u);
    EXPECT_EQ(M::EyeOf(M::View::Right), 1u);
    EXPECT_TRUE(M::ViewFromNumber(0.0f) == M::View::Game);
    EXPECT_TRUE(M::ViewFromNumber(1.0f) == M::View::Left);
    EXPECT_TRUE(M::ViewFromNumber(2.0f) == M::View::Right);
    for (const auto v : {M::View::Game, M::View::Left, M::View::Right}) {
        EXPECT_TRUE(M::ParseView(M::ViewName(v)) == v); // the vr.ini writer's word reads back
    }
}

TEST(DesktopMirror, CropOfATallEyeToAWideWindow) {
    // The Quest 3 eye at Link's recommended size into a 16:9 window: the full width, the middle
    // rows.
    const auto r = M::CentreCrop(1424, 1568, 1280, 720);
    EXPECT_EQ(r.width, 1424u);
    EXPECT_EQ(r.height, 801u); // 1424 * 720 / 1280 = 801
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, (1568 - 801) / 2);
}

TEST(DesktopMirror, CropOfAWideImageToATallWindow) {
    const auto r = M::CentreCrop(2000, 1000, 500, 1000);
    EXPECT_EQ(r.height, 1000u);
    EXPECT_EQ(r.width, 500u);
    EXPECT_EQ(r.x, 750);
    EXPECT_EQ(r.y, 0);
}

TEST(DesktopMirror, CropKeepsTheWindowShapeAndStaysInside) {
    const u32 sizes[][2] = {{1424, 1568}, {1109, 1200}, {1252, 1353}, {2720, 1808}, {1, 1},
                            {3, 1000},    {1000, 3},    {997, 1098}};
    const u32 windows[][2] = {{1280, 720}, {1920, 1080}, {800, 800}, {720, 1280},
                              {1, 1},      {3440, 1440}, {640, 481}};
    for (const auto& s : sizes) {
        for (const auto& w : windows) {
            const auto r = M::CentreCrop(s[0], s[1], w[0], w[1]);
            ASSERT_GE(r.width, 1u);
            ASSERT_GE(r.height, 1u);
            EXPECT_GE(r.x, 0);
            EXPECT_GE(r.y, 0);
            EXPECT_LE(static_cast<u32>(r.x) + r.width, s[0]);
            EXPECT_LE(static_cast<u32>(r.y) + r.height, s[1]);
            // Centred (to the pixel) and one side is the whole image.
            EXPECT_LE(std::abs(static_cast<int>(s[0] - r.width) - 2 * r.x), 1);
            EXPECT_LE(std::abs(static_cast<int>(s[1] - r.height) - 2 * r.y), 1);
            EXPECT_TRUE(r.width == s[0] || r.height == s[1]);
            // The window's shape within the rounding of the computed side (a pixel; at most the
            // window's size in the cross product when that side is clamped up to 1 pixel).
            const long long w0 = w[0], w1 = w[1];
            if (r.width == s[0]) {
                EXPECT_LE(std::llabs(static_cast<long long>(r.height) * w0 - s[0] * w1), w0);
            } else {
                EXPECT_LE(std::llabs(static_cast<long long>(r.width) * w1 - s[1] * w0), w1);
            }
        }
    }
}

TEST(DesktopMirror, SameShapeIsTheWholeImage) {
    const auto r = M::CentreCrop(1600, 900, 1920, 1080);
    EXPECT_TRUE(r == (M::Rect{0, 0, 1600, 900}));
}

TEST(DesktopMirror, ZeroSizesGiveTheWholeImage) {
    EXPECT_TRUE(M::CentreCrop(100, 50, 0, 10) == (M::Rect{0, 0, 100, 50}));
    EXPECT_TRUE(M::CentreCrop(100, 50, 10, 0) == (M::Rect{0, 0, 100, 50}));
    EXPECT_TRUE(M::CentreCrop(0, 50, 10, 10) == (M::Rect{0, 0, 0, 50}));
}

// A model of what Vulkan does to 4 bytes [b0 b1 b2 b3]: a blit moves channels by name (R, G, B,
// A) between the two declared formats, a copy moves the bytes.
struct Px {
    u8 b[4];
};
// Channel name -> byte index for RGBA and BGRA declared formats.
constexpr int Idx(bool bgra, int channel /*0 R, 1 G, 2 B, 3 A*/) {
    if (!bgra || channel == 1 || channel == 3) {
        return channel;
    }
    return channel == 0 ? 2 : 0;
}
Px Blit(Px src, bool src_bgra, bool dst_bgra) {
    Px d{};
    for (int c = 0; c < 4; ++c) {
        d.b[Idx(dst_bgra, c)] = src.b[Idx(src_bgra, c)];
    }
    return d;
}

TEST(DesktopMirror, EyeBytesReachTheWindowInItsOrder) {
    // The real colour: R=10, G=20, B=30, A=255.
    for (const bool eye_bgra : {false, true}) {
        for (const bool window_bgra : {false, true}) {
            // The eye image is declared R8G8B8A8 and holds the headset swapchain's byte order.
            const Px eye = eye_bgra ? Px{{30, 20, 10, 255}} : Px{{10, 20, 30, 255}};
            const bool mid_bgra = M::IntermediateBgra(eye_bgra, window_bgra);
            const Px mid = Blit(eye, /*src declared*/ false, mid_bgra);
            const Px window = mid; // the copy keeps the bytes
            // The window reads its bytes by its own format: R, G, B must be the real colour.
            EXPECT_EQ(window.b[Idx(window_bgra, 0)], 10) << eye_bgra << window_bgra;
            EXPECT_EQ(window.b[Idx(window_bgra, 1)], 20);
            EXPECT_EQ(window.b[Idx(window_bgra, 2)], 30) << eye_bgra << window_bgra;
            EXPECT_EQ(window.b[3], 255);
        }
    }
}

} // namespace
