// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <gtest/gtest.h>
#include "core/vr/color_settings.h"
#include "core/vr/vr_ini.h"

namespace {
using namespace VR::Color;

class VrColor : public testing::Test {
    void SetUp() override { Reset(); }
    void TearDown() override { Reset(); }
    static void Reset() {
        Set(Control::Contrast, "0");
        Set(Control::Saturation, "0");
        Set(Control::Gamma, "0");
    }
};

TEST_F(VrColor, NeutralBypassesAndGammaDirectionIsCorrect) {
    EXPECT_FALSE(Get().Active());
    EXPECT_EQ(Get().Push(), (std::array<float, 4>{1, 1, 1, 0}));
    Set(Control::Gamma, "25");
    EXPECT_TRUE(Get().Active());
    EXPECT_FLOAT_EQ(Get().Push()[2], 0.8f);
    EXPECT_FLOAT_EQ(Get().Push(true)[3], 1.0f);
}

TEST_F(VrColor, PackedWarpSettingsPreserveEverySupportedValue) {
    EXPECT_EQ(Settings{}.Packed(), 0u);
    for (int contrast = -50; contrast <= 50; ++contrast) {
        for (int saturation = -100; saturation <= 100; ++saturation) {
            for (int gamma = -50; gamma <= 50; ++gamma) {
                const Settings settings{contrast, saturation, gamma};
                const auto packed = settings.Packed();
                if (!settings.Active()) {
                    EXPECT_EQ(packed, 0u);
                    continue;
                }
                ASSERT_NE(packed, 0u);
                ASSERT_LT(packed, 1u << 24);
                ASSERT_EQ(int(packed & 255u) - 100, contrast);
                ASSERT_EQ(int((packed >> 8) & 255u) - 100, saturation);
                ASSERT_EQ(int((packed >> 16) & 255u) - 100, gamma);
            }
        }
    }
}

TEST_F(VrColor, ClampsIniValuesAndRejectsMalformedInput) {
    Set(Control::Contrast, "1000");
    Set(Control::Saturation, "-1000");
    Set(Control::Gamma, "-1000");
    EXPECT_EQ(Get().contrast, 50);
    EXPECT_EQ(Get().saturation, -100);
    EXPECT_EQ(Get().gamma, -50);
    for (const auto value : {"nan", "inf", "1.5", "25junk", "", "9999999999999"}) {
        EXPECT_THROW(Set(Control::Gamma, value), std::invalid_argument);
        EXPECT_EQ(Get().gamma, -50);
    }
}

TEST_F(VrColor, ConcurrentUpdatesDoNotLoseOtherControls) {
    std::thread a([] { for (int i = 0; i < 10000; ++i) Set(Control::Contrast, "25"); });
    std::thread b([] { for (int i = 0; i < 10000; ++i) Set(Control::Saturation, "-30"); });
    std::thread c([] { for (int i = 0; i < 10000; ++i) Set(Control::Gamma, "10"); });
    a.join(); b.join(); c.join();
    const auto value = Get();
    EXPECT_EQ(value.contrast, 25);
    EXPECT_EQ(value.saturation, -30);
    EXPECT_EQ(value.gamma, 10);
}

TEST_F(VrColor, IniRoundTripPreservesIndependentSettings) {
    const auto ini = VR::Ini::ApplyEdits("unknown=keep\n",
        {{"color_contrast", "10"}, {"color_saturation", "15"}, {"color_gamma", "-5"}});
    Set(Control::Contrast, *VR::Ini::ValueOf(ini, "color_contrast"));
    Set(Control::Saturation, *VR::Ini::ValueOf(ini, "color_saturation"));
    Set(Control::Gamma, *VR::Ini::ValueOf(ini, "color_gamma"));
    EXPECT_EQ(Get().contrast, 10);
    EXPECT_EQ(Get().saturation, 15);
    EXPECT_EQ(Get().gamma, -5);
    EXPECT_EQ(VR::Ini::ValueOf(ini, "unknown"), "keep");
}
}
