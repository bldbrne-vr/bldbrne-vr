// SPDX-License-Identifier: GPL-2.0-or-later

// The lock-on marker on the UI layer (src/core/vr/ui_lock_marker.h, mirrored by
// vr_ui_resolve.comp modes 8 and 1): the factors, their push-constant packing, the scale about
// the marker box's centre, the dimmed effect and the marker put under the rest of the UI.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/ui_lock_marker.h"

namespace {

using namespace VR::Ui::LockMarker;

TEST(UiLockMarker, FactorsClampAndActive) {
    const Factors def = Clamp(0.88f, 0.82f); // the defaults (the user: "very slight")
    EXPECT_NEAR(def.scale, 0.88f, 1e-6f);
    EXPECT_NEAR(def.alpha, 0.82f, 1e-6f);
    EXPECT_TRUE(Active(def));
    EXPECT_FALSE(Active(Clamp(1.0f, 1.0f))); // 1 / 1: exactly the game's marker, no own pair
    EXPECT_TRUE(Active(Clamp(1.0f, 0.9f)));
    EXPECT_TRUE(Active(Clamp(0.9f, 1.0f)));
    EXPECT_TRUE(Active(Clamp(1.2f, 1.0f)));
    const Factors lo = Clamp(0.0f, -1.0f);
    EXPECT_NEAR(lo.scale, 0.25f, 1e-6f);
    EXPECT_NEAR(lo.alpha, 0.0f, 1e-6f);
    const Factors hi = Clamp(9.0f, 3.0f);
    EXPECT_NEAR(hi.scale, 2.0f, 1e-6f);
    EXPECT_NEAR(hi.alpha, 1.0f, 1e-6f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Factors bad = Clamp(nan, nan);
    EXPECT_NEAR(bad.scale, 1.0f, 1e-6f);
    EXPECT_NEAR(bad.alpha, 1.0f, 1e-6f);
    EXPECT_FALSE(Active(bad));
}

TEST(UiLockMarker, PackKeepsTheChannelOrderBits) {
    for (const float s : {0.25f, 0.5f, 0.7f, 0.88f, 1.0f, 1.37f, 2.0f}) {
        for (const float a : {0.0f, 0.3f, 0.6f, 0.82f, 1.0f}) {
            const std::uint32_t bits = Pack({s, a});
            EXPECT_EQ(bits & 0xFFu, 0u); // raw_order's own byte is untouched
            const Factors back = Unpack(bits | 0xA5u);
            EXPECT_NEAR(back.scale, s, 0.5f / 1024.0f + 1e-6f);
            EXPECT_NEAR(back.alpha, a, 0.5f / 4095.0f + 1e-6f);
        }
    }
}

TEST(UiLockMarker, CentreAndSourceKeepTheMarkerInPlace) {
    const Box b{100, 40, 139, 61}; // 40 x 22 px
    const Point c = Centre(b);
    EXPECT_NEAR(c.x, 120.0f, 1e-6f);
    EXPECT_NEAR(c.y, 51.0f, 1e-6f);
    const Point one = Centre(Box{5, 7, 5, 7});
    EXPECT_NEAR(one.x, 5.5f, 1e-6f);
    EXPECT_NEAR(one.y, 7.5f, 1e-6f);
    for (const float s : {0.25f, 0.7f, 0.88f, 1.0f, 1.5f}) {
        // The centre samples itself: the marker stays on its target.
        const Point at = Source(c, c, s);
        EXPECT_NEAR(at.x, c.x, 1e-5f);
        EXPECT_NEAR(at.y, c.y, 1e-5f);
        // The scaled box's edges sample the box's own edges.
        const Point edge = Source({c.x + s * (100.0f - c.x), c.y + s * (62.0f - c.y)}, c, s);
        EXPECT_NEAR(edge.x, 100.0f, 1e-4f);
        EXPECT_NEAR(edge.y, 62.0f, 1e-4f);
    }
}

// Every output pixel outside OutputBox samples only outside the bilinear footprint of the box's
// pixels: nothing of the marker is cut off by the resolve's early out.
TEST(UiLockMarker, OutputBoxCoversEverySampleOfTheMarker) {
    const std::array<Box, 4> boxes{Box{0, 0, 0, 0}, Box{100, 40, 139, 61}, Box{7, 9, 30, 10},
                                   Box{1203, 700, 1260, 757}};
    for (const Box& b : boxes) {
        for (const float s : {0.25f, 0.6f, 0.88f, 1.0f, 1.3f, 2.0f}) {
            const Box o = OutputBox(b, s);
            const Point c = Centre(b);
            for (std::int32_t y = o.y0 - 40; y <= o.y1 + 40; ++y) {
                for (std::int32_t x = o.x0 - 40; x <= o.x1 + 40; ++x) {
                    if (x >= o.x0 && x <= o.x1 && y >= o.y0 && y <= o.y1) {
                        continue;
                    }
                    const Point src = Source({x + 0.5f, y + 0.5f}, c, s);
                    // The four taps around src - 0.5 reach pixels floor(src - 0.5) .. + 1.
                    const float sx = src.x - 0.5f;
                    const float sy = src.y - 0.5f;
                    const bool touches = std::floor(sx) + 1.0f >= static_cast<float>(b.x0) &&
                                         std::floor(sx) <= static_cast<float>(b.x1) &&
                                         std::floor(sy) + 1.0f >= static_cast<float>(b.y0) &&
                                         std::floor(sy) <= static_cast<float>(b.y1);
                    if (touches) {
                        // A touch with zero weight is fine (exactly on a pixel centre).
                        const Effect e = Sample(src, b, [](std::int32_t, std::int32_t) {
                            return Effect{1.0f, 0.0f};
                        });
                        EXPECT_NEAR(e.k, 0.0f, 1e-4f);
                    }
                }
            }
        }
    }
}

TEST(UiLockMarker, DimIsTheDrawsEffectTimesA) {
    const std::array<Effect, 4> effects{Effect{0.3f, 0.6f}, // over: alpha 0.4, colour 0.75
                                        Effect{0.45f, 1.0f}, // additive glow
                                        Effect{0.0f, 0.2f},  // black dim
                                        Effect{0.9f, 0.0f}}; // opaque
    for (const Effect& e : effects) {
        for (const float a : {0.0f, 0.5f, 0.82f, 1.0f}) {
            const Effect d = Dim(e, a);
            for (const float dst : {0.0f, 0.25f, 0.5f, 1.0f}) {
                EXPECT_NEAR(d.Apply(dst), dst + a * (e.Apply(dst) - dst), 1e-6f);
            }
        }
        const Effect same = Dim(e, 1.0f);
        EXPECT_NEAR(same.k, e.k, 1e-7f);
        EXPECT_NEAR(same.t, e.t, 1e-7f);
        const Effect none = Dim(e, 0.0f);
        EXPECT_NEAR(none.k, 0.0f, 1e-7f);
        EXPECT_NEAR(none.t, 1.0f, 1e-7f);
    }
    // An over blend of alpha 0.4 dimmed by 0.82 is an over blend of alpha 0.328, same colour.
    const Effect over = Dim(Effect{0.4f * 0.75f, 0.6f}, 0.82f);
    EXPECT_NEAR(1.0f - over.t, 0.328f, 1e-6f);
    EXPECT_NEAR(over.k / (1.0f - over.t), 0.75f, 1e-5f);
}

TEST(UiLockMarker, UnderIsTheMarkerDrawnFirst) {
    const Effect marker{0.5f, 0.0f}; // opaque
    const Effect rest{0.2f, 0.5f};   // a translucent UI pixel over it
    const Effect both = Under(rest, marker);
    for (const float dst : {0.0f, 0.3f, 1.0f}) {
        EXPECT_NEAR(both.Apply(dst), rest.Apply(marker.Apply(dst)), 1e-6f);
    }
    // No UI over the marker: the marker alone.
    const Effect alone = Under(Effect{0.0f, 1.0f}, marker);
    EXPECT_NEAR(alone.k, marker.k, 1e-7f);
    EXPECT_NEAR(alone.t, marker.t, 1e-7f);
}

// The game draws the marker last (over the target's HP bar): the dimmed marker over the rest.
TEST(UiLockMarker, OverIsTheMarkerDrawnLast) {
    const Effect glow = Dim(Effect{0.35f, 1.0f}, 0.82f); // additive glow over the bar
    const Effect bar{0.02f, 0.1f};                        // the bar's dark background
    const Effect both = Under(glow, bar);
    for (const float dst : {0.0f, 0.4f, 1.0f}) {
        EXPECT_NEAR(both.Apply(dst), glow.Apply(bar.Apply(dst)), 1e-6f);
    }
    // The glow still shows over the bar (under it, the bar would hide 90% of it).
    EXPECT_NEAR(both.k, 0.02f + 0.82f * 0.35f, 1e-6f);
    EXPECT_GT(both.k, Under(bar, glow).k + 0.2f);
}

TEST(UiLockMarker, SampleIsBilinearAndEmptyOutside) {
    const Box b{10, 10, 19, 19};
    const auto ramp = [](std::int32_t x, std::int32_t y) {
        return Effect{0.01f * static_cast<float>(x) + 0.001f * static_cast<float>(y), 0.5f};
    };
    // Pixel centres return the pixel itself.
    const Effect e = Sample({12.5f, 15.5f}, b, ramp);
    EXPECT_NEAR(e.k, 0.12f + 0.015f, 1e-6f);
    EXPECT_NEAR(e.t, 0.5f, 1e-6f);
    // Half way between two pixels: their mean.
    const Effect m = Sample({13.0f, 15.5f}, b, ramp);
    EXPECT_NEAR(m.k, 0.125f + 0.015f, 1e-6f);
    // Far outside: empty (no emission, all transmitted).
    const Effect out = Sample({40.0f, 40.0f}, b, ramp);
    EXPECT_NEAR(out.k, 0.0f, 1e-7f);
    EXPECT_NEAR(out.t, 1.0f, 1e-7f);
    // Across the box's edge the outside counts as empty: half of the edge pixel.
    const Effect edge = Sample({10.0f, 15.5f}, b, ramp);
    EXPECT_NEAR(edge.k, 0.5f * (0.10f + 0.015f), 1e-6f);
    EXPECT_NEAR(edge.t, 0.75f, 1e-6f);
}

// A synthetic marker (a soft disc) through the whole path: its centroid stays where it was (on
// the target), its radius scales by s, its total emission by a * s^2.
TEST(UiLockMarker, ADiscStaysCentredSmallerAndFainter) {
    constexpr std::int32_t W = 96;
    constexpr std::int32_t H = 80;
    const float cx = 47.3f;
    const float cy = 36.8f;
    const float r = 14.0f;
    std::vector<Effect> img(W * H);
    Box box{W, H, -1, -1};
    double k0 = 0.0, x0 = 0.0, y0 = 0.0;
    for (std::int32_t y = 0; y < H; ++y) {
        for (std::int32_t x = 0; x < W; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            img[y * W + x] = {0.9f * cov, 1.0f - cov};
            if (cov > 0.0f) {
                box.x0 = std::min(box.x0, x);
                box.y0 = std::min(box.y0, y);
                box.x1 = std::max(box.x1, x);
                box.y1 = std::max(box.y1, y);
                k0 += 0.9 * cov;
                x0 += 0.9 * cov * (x + 0.5);
                y0 += 0.9 * cov * (y + 0.5);
            }
        }
    }
    ASSERT_TRUE(box.Valid());
    const Point c = Centre(box);
    const auto fetch = [&](std::int32_t x, std::int32_t y) { return img[y * W + x]; };
    for (const Factors f : {Factors{1.0f, 1.0f}, Factors{0.88f, 0.82f}, Factors{0.7f, 0.6f}}) {
        const Box o = OutputBox(box, f.scale);
        double k = 0.0, mx = 0.0, my = 0.0;
        float k_max = 0.0f;
        for (std::int32_t y = std::max(o.y0, 0); y <= std::min(o.y1, H - 1); ++y) {
            for (std::int32_t x = std::max(o.x0, 0); x <= std::min(o.x1, W - 1); ++x) {
                const Effect e =
                    Dim(Sample(Source({x + 0.5f, y + 0.5f}, c, f.scale), box, fetch), f.alpha);
                k += e.k;
                mx += e.k * (x + 0.5);
                my += e.k * (y + 0.5);
                k_max = std::max(k_max, e.k);
            }
        }
        // Scaled about the box's centre: the disc's centroid moves to c + s * (centroid - c),
        // i.e. at most (1 - s) / 2 px (the box is whole pixels): it stays on its target.
        const double ex = c.x + f.scale * (x0 / k0 - c.x);
        const double ey = c.y + f.scale * (y0 / k0 - c.y);
        EXPECT_NEAR(mx / k, ex, 0.02);
        EXPECT_NEAR(my / k, ey, 0.02);
        EXPECT_LE(std::abs(mx / k - x0 / k0), 0.5 * (1.0 - f.scale) + 0.02);
        EXPECT_LE(std::abs(my / k - y0 / k0), 0.5 * (1.0 - f.scale) + 0.02);
        // Area times s^2, brightness times a.
        EXPECT_NEAR(k / k0, f.alpha * f.scale * f.scale, 0.02 * f.alpha);
        EXPECT_NEAR(k_max, 0.9f * f.alpha, 0.01f);
    }
}

} // namespace
