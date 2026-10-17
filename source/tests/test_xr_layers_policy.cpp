// SPDX-License-Identifier: GPL-2.0-or-later

// The base layer, the HUD's cut, the layer kinds' text and the XrTime offset
// (renderer_vulkan/vr_xr_layers_policy.h). The runtimes are the user's logs' (2026-10-15):
// SteamVR without XR_KHR_composition_layer_color_scale_bias, Meta's (and the mock) with it.

#include <array>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_xr_layers_policy.h"

namespace {

namespace L = Vulkan::VrXrLayers;
using Vulkan::VrXrQueuePolicy::Runtime;

TEST(XrLayersPolicy, BaseLayerOnlyWithoutTheColourScaleExtension) {
    EXPECT_TRUE(L::ChooseBaseLayer(nullptr, false).on);  // SteamVR
    EXPECT_FALSE(L::ChooseBaseLayer(nullptr, true).on);  // Meta's, the mock: as before
    EXPECT_TRUE(L::ChooseBaseLayer("", false).on);       // empty: unset
    EXPECT_FALSE(L::ChooseBaseLayer("", true).on);
    EXPECT_TRUE(L::ChooseBaseLayer("auto", false).on);   // another word: the default
    EXPECT_FALSE(L::ChooseBaseLayer("auto", true).on);
}

TEST(XrLayersPolicy, BaseLayerSwitch) {
    for (const char* off : {"0", "off", "False", "NO"}) {
        EXPECT_FALSE(L::ChooseBaseLayer(off, false).on) << off;
        EXPECT_FALSE(L::ChooseBaseLayer(off, true).on) << off;
        EXPECT_EQ(L::ChooseBaseLayer(off, false).reason, "VR_XR_BASE_LAYER is off");
    }
    for (const char* on : {"1", "on", "TRUE", "yes"}) {
        EXPECT_TRUE(L::ChooseBaseLayer(on, false).on) << on;
        EXPECT_TRUE(L::ChooseBaseLayer(on, true).on) << on; // a test on Meta's or the mock
    }
}

TEST(XrLayersPolicy, BaseUnderOnlyWithoutTheEyes) {
    EXPECT_TRUE(L::BaseUnder(true, true, false));   // the screen, a panel alone, a fade's black
    EXPECT_FALSE(L::BaseUnder(true, true, true));   // the eyes cover the view
    EXPECT_FALSE(L::BaseUnder(true, false, false)); // not rendered: no layers at all
    EXPECT_FALSE(L::BaseUnder(false, true, false)); // not made (Meta's)
    EXPECT_TRUE(L::BaseAlone(true, true));
    EXPECT_FALSE(L::BaseAlone(false, true));
    EXPECT_FALSE(L::BaseAlone(true, false));
}

TEST(XrLayersPolicy, BaseProbeOffAfterTwoAcceptedProbes) {
    using V = L::BaseProbe::Verdict;
    L::BaseProbe p;
    EXPECT_FALSE(p.probe);
    p.Rejected(); // base+screen+overlay rejected
    EXPECT_TRUE(p.probe);
    EXPECT_EQ(p.Probed(true), V::Again); // screen+overlay accepted
    EXPECT_FALSE(p.probe);
    p.Rejected(); // with it again: rejected
    EXPECT_EQ(p.Probed(true), V::Off);
}

TEST(XrLayersPolicy, BaseProbeNotTheCause) {
    using V = L::BaseProbe::Verdict;
    L::BaseProbe p;
    p.Rejected();
    EXPECT_EQ(p.Probed(true), V::Again);
    // An accepted frame with the base layer in between: the strikes start over.
    p.Accepted();
    p.Rejected();
    EXPECT_EQ(p.Probed(true), V::Again);
    // Rejected without it too: the base layer was not the cause, the strikes start over.
    p.Rejected();
    EXPECT_EQ(p.Probed(false), V::NotCause);
    EXPECT_FALSE(p.probe);
    p.Rejected();
    EXPECT_EQ(p.Probed(true), V::Again);
}

TEST(XrLayersPolicy, InsertFirstMovesTheLayersUp) {
    const int a = 1, b = 2, c = 3, base = 0;
    std::array<const int*, 4> layers{&a, &b, &c, nullptr};
    u32 count = 3;
    EXPECT_TRUE(L::InsertFirst(layers, count, &base));
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(layers[0], &base);
    EXPECT_EQ(layers[1], &a);
    EXPECT_EQ(layers[2], &b);
    EXPECT_EQ(layers[3], &c);
    // Full: nothing changes.
    EXPECT_FALSE(L::InsertFirst(layers, count, &base));
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(layers[1], &a);
    // Empty: the base alone.
    std::array<const int*, 12> none{};
    u32 zero = 0;
    EXPECT_TRUE(L::InsertFirst(none, zero, &base));
    EXPECT_EQ(zero, 1u);
    EXPECT_EQ(none[0], &base);
}

TEST(XrLayersPolicy, BaseFovHasAMarginWithinLimits) {
    // A Quest 3 eye (SteamVR's located FOV, about): left -0.94, right 0.73, up 0.82, down -0.87.
    const L::Fov eye{-0.94f, 0.73f, 0.82f, -0.87f};
    const L::Fov wide = L::WidenFov(eye);
    EXPECT_NEAR(wide.left, eye.left - L::BaseFovMarginRad, 1e-6f);
    EXPECT_NEAR(wide.right, eye.right + L::BaseFovMarginRad, 1e-6f);
    EXPECT_NEAR(wide.up, eye.up + L::BaseFovMarginRad, 1e-6f);
    EXPECT_NEAR(wide.down, eye.down - L::BaseFovMarginRad, 1e-6f);
    // Never past 85 degrees.
    const L::Fov huge = L::WidenFov({-1.5f, 1.5f, 1.5f, -1.5f});
    EXPECT_FLOAT_EQ(huge.left, -L::MaxHalfAngleRad);
    EXPECT_FLOAT_EQ(huge.right, L::MaxHalfAngleRad);
    EXPECT_FLOAT_EQ(huge.up, L::MaxHalfAngleRad);
    EXPECT_FLOAT_EQ(huge.down, -L::MaxHalfAngleRad);
}

TEST(XrLayersPolicy, KindsText) {
    EXPECT_EQ(L::KindsText(0), "none");
    EXPECT_EQ(L::KindsText(L::Base), "base");
    EXPECT_EQ(L::KindsText(L::Base | L::Screen | L::Overlay), "base+screen+overlay");
    EXPECT_EQ(L::KindsText(L::Projection | L::Ui | L::Overlay), "projection+ui+overlay");
    EXPECT_EQ(L::KindsText(L::Projection | L::Panel | L::Page), "projection+panel+page");
}

TEST(XrLayersPolicy, HudHandCheckAndCut) {
    // Meta's: as before (the check with the fade only, never a cut).
    EXPECT_TRUE(L::UiHandCheck(true, true));
    EXPECT_FALSE(L::UiHandCheck(false, true));
    EXPECT_FALSE(L::UiCut(true, 0.0f));
    EXPECT_FALSE(L::UiCut(true, 0.15f));
    // SteamVR: the check always, a piece below half cut.
    EXPECT_TRUE(L::UiHandCheck(false, false));
    EXPECT_TRUE(L::UiCut(false, 0.15f)); // a hand in front (ui_hand_fade's 15%)
    EXPECT_TRUE(L::UiCut(false, 0.499f));
    EXPECT_FALSE(L::UiCut(false, 0.5f));
    EXPECT_FALSE(L::UiCut(false, 1.0f));
}

TEST(XrLayersPolicy, ClockOffsetFromTheTightestSample) {
    // XrTime 5 ms ahead of the steady clock; the second sample was read in 100 ns, the others
    // were preempted (the midpoint of a long window is off).
    const std::array<L::ClockSample, 3> samples{{
        {1'000'000, 1'900'000, 6'100'000},
        {2'000'000, 2'000'100, 7'000'050},
        {3'000'000, 3'500'000, 8'000'000},
    }};
    EXPECT_EQ(L::ClockOffset(samples), 5'000'000);
    // The same clock: 0.
    const std::array<L::ClockSample, 1> same{{{100, 300, 200}}};
    EXPECT_EQ(L::ClockOffset(same), 0);
}

TEST(XrLayersPolicy, ClockOffsetAppliedFromOneMillisecondOffMeta) {
    EXPECT_FALSE(L::ApplyClockOffset(0, Runtime::SteamVr));
    EXPECT_FALSE(L::ApplyClockOffset(999'999, Runtime::SteamVr));
    EXPECT_FALSE(L::ApplyClockOffset(-999'999, Runtime::Other));
    EXPECT_TRUE(L::ApplyClockOffset(1'000'000, Runtime::SteamVr));
    EXPECT_TRUE(L::ApplyClockOffset(-25'000'000, Runtime::Other));
    // Meta's runtime (and the mock): never, its times as before.
    EXPECT_FALSE(L::ApplyClockOffset(1'000'000, Runtime::Meta));
    EXPECT_FALSE(L::ApplyClockOffset(-50'000'000'000, Runtime::Meta));
}

} // namespace
