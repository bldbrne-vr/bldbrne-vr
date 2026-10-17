// SPDX-License-Identifier: GPL-2.0-or-later

// The UI layer's pixels without UI (src/core/vr/ui_empty_pixel.h, mirrored by
// vr_ui_resolve.comp EmptyPixel / ReadKT, vr.ini ui_empty_snap): K and W at their clear values to
// within one 8-bit step are exactly transparent; the grey's rounding no longer leaves a black
// alpha of 2/255 there; every other pixel resolves exactly as before, and whatever an empty pixel
// drops was about an LSB and a half at most in the game's own image.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "core/vr/ui_empty_pixel.h"
#include "core/vr/ui_lock_marker.h"

namespace {

using namespace VR::Ui::Empty;

constexpr float Grey = 0.5f; // vr.ini ui_grey's default

Rgb Bytes(int r, int g, int b) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f};
}
Rgb Bytes(int v) {
    return Bytes(v, v, v);
}

bool Same(const Pixel& a, const Pixel& b) {
    return a.rgb == b.rgb && a.alpha == b.alpha;
}
int Alpha(const Pixel& p) {
    return static_cast<int>(p.alpha);
}
bool NoColour(const Pixel& p) {
    return p.rgb[0] == 0 && p.rgb[1] == 0 && p.rgb[2] == 0;
}

// The floor: no UI drew, W holds the grey as the GPU stored it. 127 (0.5 rounded down, this
// machine's GPU) read as T = 127 / 127.5: alpha 2/255, black. Now exactly transparent.
TEST(UiEmptyPixel, TheGreysRoundingIsNoLongerAVeil) {
    const Pixel before = Resolve(Bytes(0), Bytes(127), Grey, false, false);
    EXPECT_EQ(Alpha(before), 2); // the measured floor (mock dumps: 86% of the rest quad)
    EXPECT_TRUE(NoColour(before));
    const Pixel after = Resolve(Bytes(0), Bytes(127), Grey, false, true);
    EXPECT_EQ(Alpha(after), 0);
    EXPECT_TRUE(NoColour(after));
    // 0.5 rounded up (128): T clamps to 1, transparent with or without.
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(128), Grey, false, false)), 0);
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(128), Grey, false, true)), 0);
    EXPECT_TRUE(IsEmpty(Bytes(0), Bytes(127), Grey, false));
    EXPECT_TRUE(IsEmpty(Bytes(0), Bytes(128), Grey, false));
    // Through sRGB views the grey is cleared encoded: 187.5 of 255, stored 187 or 188.
    EXPECT_NEAR(GreyRaw(Grey, true) * 255.0f, 187.52f, 0.01f);
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(187), Grey, true, false)), 2);
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(187), Grey, true, true)), 0);
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(188), Grey, true, true)), 0);
    EXPECT_TRUE(IsEmpty(Bytes(0), Bytes(187), Grey, true));
    EXPECT_TRUE(IsEmpty(Bytes(0), Bytes(188), Grey, true));
}

// Only K's and W's clear values are empty: one step more on any channel is UI.
TEST(UiEmptyPixel, OnlyTheClearValuesAreEmpty) {
    EXPECT_FALSE(IsEmpty(Bytes(1), Bytes(127), Grey, false)); // a glow's last LSB
    EXPECT_FALSE(IsEmpty(Bytes(0, 0, 1), Bytes(127), Grey, false));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(126), Grey, false)); // a faint black blend
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(129), Grey, false)); // a faint additive one
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(127, 126, 127), Grey, false));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(128, 128, 129), Grey, false));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(186), Grey, true));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(189), Grey, true));
    // Not the other domain's grey (127 through sRGB views is a 57% black dim).
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(127), Grey, true));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(188), Grey, false));
    // A grey that is a whole byte (vr.ini ui_grey 0.6 = 153 of 255): that byte alone.
    EXPECT_TRUE(IsEmpty(Bytes(0), Bytes(153), 0.6f, false));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(152), 0.6f, false));
    EXPECT_FALSE(IsEmpty(Bytes(0), Bytes(154), 0.6f, false));
    // ui_empty_snap=0: the old reading, the floor included.
    EXPECT_EQ(Alpha(Resolve(Bytes(0), Bytes(127), Grey, false, false)), 2);
}

// Every pixel that is not empty resolves to the same bytes as before: all 65536 grey K / W pairs
// and coloured ones, in both blend domains.
TEST(UiEmptyPixel, EveryOtherPixelResolvesAsBefore) {
    for (const bool srgb : {false, true}) {
        int empty = 0;
        for (int k = 0; k < 256; ++k) {
            for (int w = 0; w < 256; ++w) {
                for (const auto& [kc, wc] : {std::pair{Bytes(k), Bytes(w)},
                                             std::pair{Bytes(k, k / 2, k / 3), Bytes(w, w, w / 2)},
                                             std::pair{Bytes(k / 4, k, 255 - k), Bytes(w)}}) {
                    const Pixel before = Resolve(kc, wc, Grey, srgb, false);
                    const Pixel after = Resolve(kc, wc, Grey, srgb, true);
                    if (IsEmpty(kc, wc, Grey, srgb)) {
                        ++empty;
                        EXPECT_EQ(Alpha(after), 0);
                        EXPECT_TRUE(NoColour(after));
                        EXPECT_LE(Alpha(before), 2) << k << " " << w; // only the floor goes
                        continue;
                    }
                    ASSERT_TRUE(Same(before, after)) << srgb << " " << k << " " << w;
                }
            }
        }
        EXPECT_EQ(empty, 2) << srgb; // K 0 with W at either rounding of the grey
    }
}

// What an empty pixel drops: the game's blends of faint UI onto the cleared targets, 8-bit (the
// GPU rounds each store), for both ways a GPU stores the grey. Wherever the result still reads as
// empty, that UI changed the game's own image X = K + T * S (over any scene S) by about 1.5 LSB
// at most with 127 (this machine) and 3.5 LSB with 128 (a black blend that took W from 128 to
// 127, which the old resolve read as the same 2/255 as the floor).
TEST(UiEmptyPixel, WhatAnEmptyPixelDropsWasAnLsbOrTwoInTheGame) {
    for (const int stored : {127, 128}) {
        const float bound = stored == 127 ? 1.55f : 3.5f;
        float worst = 0.0f;
        int dropped = 0;
        // Over blends: colour c at alpha a (premultiplied): K = c a, W = c a + (1 - a) W0.
        for (const float c : {0.0f, 0.02f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.4f, 0.5f,
                              0.75f, 1.0f}) {
            for (int i = 0; i <= 1000; ++i) {
                const float a = static_cast<float>(i) * 0.00005f; // up to 12.75 / 255
                const int k = static_cast<int>(std::lround(c * a * 255.0f));
                const int w = static_cast<int>(std::lround(c * a * 255.0f + (1.0f - a) * stored));
                if (!IsEmpty(Bytes(k), Bytes(w), Grey, false)) {
                    continue;
                }
                const float effect = a * std::max(c, 1.0f - c) * 255.0f; // S = 0 or 1
                worst = std::max(worst, effect);
                dropped += effect > 0.0f ? 1 : 0;
            }
        }
        // Additive light e: K = e, W = W0 + e.
        for (int i = 0; i <= 400; ++i) {
            const float e = static_cast<float>(i) * 0.0001f;
            const int k = static_cast<int>(std::lround(e * 255.0f));
            const int w = static_cast<int>(std::lround(e * 255.0f + stored));
            if (IsEmpty(Bytes(k), Bytes(w), Grey, false)) {
                worst = std::max(worst, e * 255.0f);
                dropped += e > 0.0f ? 1 : 0;
            }
        }
        EXPECT_GT(dropped, 0) << stored; // the test reaches the edge
        EXPECT_LE(worst, bound) << stored;
    }
}

// Faint UI the game really draws keeps its pixels: a menu's dim, a text shadow's fringe, an
// additive glow's fringe, a banner fading in, the enemy bar's dark back.
TEST(UiEmptyPixel, FaintUiKeepsItsPixels) {
    struct Case {
        const char* what;
        Rgb k, w;
    };
    const Case cases[] = {
        {"menu dim 60%", Bytes(0), Bytes(51)},
        {"text shadow fringe 4/255", Bytes(0), Bytes(125)},
        {"glow fringe", Bytes(2), Bytes(129)},
        {"glow fringe, one LSB", Bytes(1), Bytes(128)},
        {"banner fading in, 3/255 of grey 190", Bytes(2), Bytes(128)},
        {"bar back 85% dark red", Bytes(30, 4, 4), Bytes(49, 23, 23)},
    };
    for (const Case& c : cases) {
        const Pixel before = Resolve(c.k, c.w, Grey, false, false);
        const Pixel after = Resolve(c.k, c.w, Grey, false, true);
        EXPECT_FALSE(IsEmpty(c.k, c.w, Grey, false)) << c.what;
        EXPECT_TRUE(Same(before, after)) << c.what;
        EXPECT_TRUE(Alpha(after) > 0 || !NoColour(after)) << c.what;
    }
}

// The lock-on marker's box: its own pair (Km, Wm) has the same floor, and the resolve puts it,
// dimmed (ui_lock_marker_alpha 0.82), over the rest's floor: alpha 4/255 over the whole box (the
// faint box the mock showed). Both reads are exact now: the box is transparent where neither
// drew.
TEST(UiEmptyPixel, TheLockOnMarkersBoxLosesItsDoubleFloor) {
    using VR::Ui::LockMarker::Dim;
    using VR::Ui::LockMarker::Effect;
    for (const bool snap : {false, true}) {
        Rgb k = Bytes(0);
        const Rgb t = ReadKT(k, Bytes(127), Grey, false, snap); // the rest
        Rgb km = Bytes(0);
        const Rgb tm = ReadKT(km, Bytes(127), Grey, false, snap); // the marker's own pair
        Rgb k_out{}, t_out{};
        for (int c = 0; c < 3; ++c) {
            const Effect m = Dim({km[c], tm[c]}, 0.82f);
            k_out[c] = m.k + m.t * k[c]; // the marker over the rest
            t_out[c] = t[c] * m.t;
        }
        const Pixel p = FromKT(k_out, t_out, false);
        EXPECT_EQ(Alpha(p), snap ? 0 : 4);
        EXPECT_TRUE(NoColour(p));
    }
}

} // namespace
