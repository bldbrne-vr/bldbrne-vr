// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini eye_size (renderer_vulkan/vr_eye_scale.h): the eye images' size for the headset from
// their crops of the render. The numbers are the Quest 3 on Meta Link (the user's log: recommended
// 1424x1568, max 2848x3136) and the mock runtime (recommended 2064x2208, largest swapchain 4096).

#include <array>
#include <ostream>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_eye_scale.h"

namespace Vulkan::VrEyeScale {
// For the failure messages.
inline std::ostream& operator<<(std::ostream& os, const Size& s) {
    return os << s.width << "x" << s.height;
}
} // namespace Vulkan::VrEyeScale

namespace {

namespace ES = Vulkan::VrEyeScale;

// 2560x1920 with fov_margin_deg 6: each eye's crop.
constexpr std::array<ES::Size, 2> Crops{{{1178, 1274}, {1178, 1274}}};
constexpr std::array<ES::Size, 2> LinkRecommended{{{1424, 1568}, {1424, 1568}}};
constexpr std::array<ES::Size, 2> MockRecommended{{{2064, 2208}, {2064, 2208}}};
constexpr ES::Size LinkMaxRect{2848, 3136};
constexpr ES::Size Big{16384, 16384};

TEST(EyeScale, ScaledKeepsSizeAtOneOrLess) {
    EXPECT_EQ(ES::Scaled({1178, 1274}, 1.0f), (ES::Size{1178, 1274}));
    EXPECT_EQ(ES::Scaled({1178, 1274}, 0.5f), (ES::Size{1178, 1274}));
}

TEST(EyeScale, ScaledIsEvenAndNeverSmaller) {
    const ES::Size s = ES::Scaled({1179, 1275}, 1.5f);
    EXPECT_EQ(s.width % 2, 0u);
    EXPECT_EQ(s.height % 2, 0u);
    EXPECT_NEAR(static_cast<double>(s.width), 1179 * 1.5, 1.0);
    EXPECT_NEAR(static_cast<double>(s.height), 1275 * 1.5, 1.0);
    EXPECT_EQ(ES::Scaled({1178, 1274}, 1.5f), (ES::Size{1768, 1912})); // the measured runs
    EXPECT_EQ(ES::Scaled({100, 100}, 99.0f), (ES::Size{400, 400}));    // at most 4x
}

TEST(EyeScale, FixedSettingIsUsed) {
    EXPECT_FLOAT_EQ(ES::Factor(1.5f, Crops, LinkRecommended, Big, LinkMaxRect), 1.5f);
    EXPECT_FLOAT_EQ(ES::Factor(1.0f, Crops, LinkRecommended, Big, LinkMaxRect), 1.0f);
}

TEST(EyeScale, AutoFollowsTheRecommendedSize) {
    // Link today: max(1424 / 1178, 1568 / 1274) = 1.2308
    EXPECT_NEAR(ES::Factor(0.0f, Crops, LinkRecommended, Big, LinkMaxRect), 1568.0 / 1274.0, 1e-4);
    // the panel-sized recommendation of the mock: 2064 / 1178 = 1.752
    EXPECT_NEAR(ES::Factor(0.0f, Crops, MockRecommended, Big, {4096, 4096}), 2064.0 / 1178.0,
                1e-4);
}

TEST(EyeScale, AutoIsAtLeastOneAndAtMostTwo) {
    // a recommendation smaller than the render's crop: as warped
    EXPECT_FLOAT_EQ(ES::Factor(0.0f, Crops, {{{800, 900}, {800, 900}}}, Big, LinkMaxRect), 1.0f);
    // Link's maximum render resolution (2704 x 2736 per eye): capped at 2
    EXPECT_FLOAT_EQ(ES::Factor(0.0f, Crops, {{{2704, 2736}, {2704, 2736}}}, Big, {8192, 8192}),
                    2.0f);
}

TEST(EyeScale, CappedByTheLargestSwapchainAndRect) {
    // the mock's 4096-wide swapchains hold both eyes side by side (plus the 64 px rounding)
    const float f = ES::Factor(2.0f, Crops, MockRecommended, {4096, 4096}, {4096, 4096});
    EXPECT_NEAR(f, (4096.0 - 64.0) / (2 * 1178.0), 1e-4); // 1.711: the 2016x2180 eyes measured
    const ES::Size s = ES::Scaled(Crops[0], f);
    EXPECT_LE(2 * s.width + 64, 4096u);
    // each eye within the runtime's largest image rect
    EXPECT_NEAR(ES::Factor(2.0f, Crops, LinkRecommended, Big, {1500, 3000}), 1500.0 / 1178.0, 1e-4);
}

TEST(EyeScale, TooLittleToBotherIsOne) {
    EXPECT_FLOAT_EQ(ES::Factor(1.01f, Crops, LinkRecommended, Big, LinkMaxRect), 1.0f);
    EXPECT_FLOAT_EQ(ES::Factor(0.0f, Crops, {{{1190, 1290}, {1190, 1290}}}, Big, LinkMaxRect), 1.0f);
}

TEST(EyeScale, EmptyCropsAreLeftAlone) {
    EXPECT_FLOAT_EQ(ES::Factor(1.5f, {{{0, 0}, {1178, 1274}}}, LinkRecommended, Big, LinkMaxRect),
                    1.0f);
    EXPECT_FLOAT_EQ(ES::Choose(ES::Mode::Panel, 0.0f, {{{1178, 1274}, {0, 1274}}}, LinkRecommended,
                               {2064, 2208}, Big, LinkMaxRect)
                        .factor,
                    1.0f);
}

// ---- eye_size=panel: the headset's own panel, whatever the Link app's slider says ----

constexpr ES::Size Quest3Panel{2064, 2208};
constexpr ES::Size LinkSwapchain{4096, 4096}; // the user's log: largest swapchain 4096x4096

TEST(EyeScale, KnownPanelsFromTheSystemName) {
    EXPECT_EQ(ES::KnownPanel("Meta Quest 3").size, Quest3Panel);
    EXPECT_EQ(ES::KnownPanel("Oculus Quest3").size, Quest3Panel);
    EXPECT_EQ(ES::KnownPanel("META QUEST 3").model, "Quest 3");
    EXPECT_EQ(ES::KnownPanel("Meta Quest 3 (Link)").size, Quest3Panel);
    EXPECT_EQ(ES::KnownPanel("Quest 3 Standalone").size, Quest3Panel); // not 3S
    EXPECT_EQ(ES::KnownPanel("Meta Quest 3S").size, (ES::Size{1832, 1920}));
    EXPECT_EQ(ES::KnownPanel("Quest 3S Xbox Edition").model, "Quest 3S");
    EXPECT_EQ(ES::KnownPanel("Oculus Quest2").size, (ES::Size{1832, 1920}));
    EXPECT_EQ(ES::KnownPanel("Meta Quest Pro").size, (ES::Size{1800, 1920}));
    EXPECT_EQ(ES::KnownPanel("Oculus Quest").size, (ES::Size{1440, 1600}));
    EXPECT_EQ(ES::KnownPanel("Oculus Rift S").size, (ES::Size{1280, 1440}));
    EXPECT_EQ(ES::KnownPanel("Oculus Rift CV1").size, (ES::Size{1080, 1200}));
    // not known: eye_size=panel follows the runtime's recommended size
    EXPECT_EQ(ES::KnownPanel("MockXR HMD").size, (ES::Size{}));
    EXPECT_EQ(ES::KnownPanel("SteamVR/OpenXR : lighthouse").size, (ES::Size{}));
    EXPECT_EQ(ES::KnownPanel("Meta Quest 4").size, (ES::Size{}));
    EXPECT_EQ(ES::KnownPanel("").size, (ES::Size{}));
    EXPECT_EQ(ES::KnownPanel("Conquest 3").size, (ES::Size{})); // a whole word only
}

TEST(EyeScale, PanelIgnoresTheLinkSlider) {
    // the 2560x1920 preset on Link at 1x (recommended 1424x1568, max 2848x3136): 2064 / 1178 =
    // 1.752, capped by the 4096 swapchain holding both eyes: (4096 - 64) / 2356 = 1.711
    const ES::Choice c = ES::Choose(ES::Mode::Panel, 0.0f, Crops, LinkRecommended, Quest3Panel,
                                    LinkSwapchain, LinkMaxRect);
    EXPECT_EQ(c.mode, ES::Mode::Panel);
    EXPECT_EQ(c.target, Quest3Panel);
    EXPECT_EQ(c.limit, ES::Limit::Swapchain);
    EXPECT_NEAR(c.factor, (4096.0 - 64.0) / (2 * 1178.0), 1e-4);
    EXPECT_EQ(ES::Scaled(Crops[0], c.factor), (ES::Size{2016, 2180}));
    // the same with the slider at 3200x1728 (recommended ~1618x1782, max 2x) and at 0.85x
    for (const ES::Size rec : {ES::Size{1618, 1782}, ES::Size{1210, 1332}}) {
        const ES::Choice d = ES::Choose(ES::Mode::Panel, 0.0f, Crops, {{rec, rec}}, Quest3Panel,
                                        LinkSwapchain, {2 * rec.width, 2 * rec.height});
        EXPECT_FLOAT_EQ(d.factor, c.factor);
    }
    // without the swapchain cap: the panel itself, 2064 / 1178
    const ES::Choice e = ES::Choose(ES::Mode::Panel, 0.0f, Crops, LinkRecommended, Quest3Panel,
                                    Big, {4096, 4096});
    EXPECT_NEAR(e.factor, 2064.0 / 1178.0, 1e-4);
    EXPECT_EQ(e.limit, ES::Limit::None);
    const ES::Size s = ES::Scaled(Crops[0], e.factor);
    EXPECT_GE(s.width, 2064u);
    EXPECT_GE(s.height, 2208u);
}

TEST(EyeScale, PanelForTheOtherPresets) {
    // SHARP (3200x2400: 1474x1592 crops): 2064 / 1474 = 1.400, swapchain (4032 / 2948) = 1.368
    const std::array<ES::Size, 2> sharp{{{1474, 1592}, {1474, 1592}}};
    const ES::Choice c = ES::Choose(ES::Mode::Panel, 0.0f, sharp, LinkRecommended, Quest3Panel,
                                    LinkSwapchain, LinkMaxRect);
    EXPECT_NEAR(c.factor, 4032.0 / 2948.0, 1e-4);
    EXPECT_EQ(c.limit, ES::Limit::Swapchain);
    // a small preset (880x956 crops): beyond auto's x2, 2064 / 880 = 2.345
    const std::array<ES::Size, 2> small{{{880, 956}, {880, 956}}};
    const ES::Choice d = ES::Choose(ES::Mode::Panel, 0.0f, small, LinkRecommended, Quest3Panel,
                                    Big, {4096, 4096});
    EXPECT_NEAR(d.factor, 2064.0 / 880.0, 1e-4);
    // ... and at most x3
    const std::array<ES::Size, 2> tiny{{{600, 650}, {600, 650}}};
    const ES::Choice e = ES::Choose(ES::Mode::Panel, 0.0f, tiny, LinkRecommended, Quest3Panel,
                                    Big, {4096, 4096});
    EXPECT_FLOAT_EQ(e.factor, ES::MaxPanelFactor);
    EXPECT_EQ(e.limit, ES::Limit::MaxFactor);
    // a render already panel-sized: as warped
    const std::array<ES::Size, 2> big{{{2100, 2250}, {2100, 2250}}};
    const ES::Choice f = ES::Choose(ES::Mode::Panel, 0.0f, big, LinkRecommended, Quest3Panel,
                                    Big, {4096, 4096});
    EXPECT_FLOAT_EQ(f.factor, 1.0f);
    EXPECT_EQ(f.limit, ES::Limit::TooSmall);
}

TEST(EyeScale, PanelNeverAboveTheLargestImageRect) {
    // the slider at 0.7x: Link's max rect (2x recommended) 1994x2196 < the panel's 2064
    const ES::Choice c = ES::Choose(ES::Mode::Panel, 0.0f, Crops, {{{997, 1098}, {997, 1098}}},
                                    Quest3Panel, Big, {1994, 2196});
    EXPECT_NEAR(c.factor, 1994.0 / 1178.0, 1e-4);
    EXPECT_EQ(c.limit, ES::Limit::ImageRect);
    const ES::Size s = ES::Scaled(Crops[0], c.factor);
    EXPECT_LE(s.width, 1994u);
    EXPECT_LE(s.height, 2196u);
}

TEST(EyeScale, PanelUnknownFollowsTheRecommended) {
    const ES::Choice c = ES::Choose(ES::Mode::Panel, 0.0f, Crops, LinkRecommended, ES::Size{},
                                    Big, LinkMaxRect);
    EXPECT_EQ(c.mode, ES::Mode::Auto);
    EXPECT_EQ(c.target, LinkRecommended[0]);
    EXPECT_NEAR(c.factor, 1568.0 / 1274.0, 1e-4);
    EXPECT_FLOAT_EQ(c.factor, ES::Factor(0.0f, Crops, LinkRecommended, Big, LinkMaxRect));
}

TEST(EyeScale, AutoAndFactorAreAsBefore) {
    const ES::Choice a = ES::Choose(ES::Mode::Auto, 0.0f, Crops, LinkRecommended, Quest3Panel,
                                    Big, LinkMaxRect);
    EXPECT_NEAR(a.factor, 1568.0 / 1274.0, 1e-4); // x1.23: 1450x1568
    EXPECT_EQ(ES::Scaled(Crops[0], a.factor), (ES::Size{1450, 1568}));
    const ES::Choice f = ES::Choose(ES::Mode::Factor, 1.5f, Crops, LinkRecommended, Quest3Panel,
                                    Big, LinkMaxRect);
    EXPECT_FLOAT_EQ(f.factor, 1.5f);
    EXPECT_EQ(f.mode, ES::Mode::Factor);
    EXPECT_EQ(f.target, (ES::Size{}));
    EXPECT_EQ(f.limit, ES::Limit::None);
    // factor 1 = off: not "too small"
    EXPECT_EQ(ES::Choose(ES::Mode::Factor, 1.0f, Crops, LinkRecommended, Quest3Panel, Big,
                         LinkMaxRect)
                  .limit,
              ES::Limit::None);
}

// ---- eye_size_min_factor: auto never shows the raw crop with RCAS alone ----

constexpr std::array<ES::Size, 2> SharpCrops{{{1474, 1592}, {1474, 1592}}}; // 3200x2400

TEST(EyeScale, FloorLeaves2560AtLinkOneXAlone) {
    for (const ES::Size panel : {Quest3Panel, ES::Size{}}) {
        const ES::Choice c = ES::Choose(ES::Mode::Auto, 0.0f, Crops, LinkRecommended, panel,
                                        LinkSwapchain, LinkMaxRect, ES::DefaultMinFactor);
        EXPECT_NEAR(c.factor, 1568.0 / 1274.0, 1e-4); // x1.231, as before
        EXPECT_FALSE(c.floored);
        EXPECT_EQ(ES::Scaled(Crops[0], c.factor), (ES::Size{1450, 1568}));
    }
}

TEST(EyeScale, FloorGives3200TheSmallEasuStep) {
    // Link 1x: recommended 1424x1568 < the 1474x1592 crop: x1.0 before (RCAS on raw pixels)
    const ES::Choice before = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops, LinkRecommended,
                                         Quest3Panel, LinkSwapchain, LinkMaxRect);
    EXPECT_FLOAT_EQ(before.factor, 1.0f);
    EXPECT_EQ(before.limit, ES::Limit::TooSmall);
    for (const ES::Size panel : {Quest3Panel, ES::Size{}}) { // the Quest 3, the mock
        const ES::Choice c = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops, LinkRecommended, panel,
                                        LinkSwapchain, LinkMaxRect, ES::DefaultMinFactor);
        EXPECT_FLOAT_EQ(c.factor, 1.13f);
        EXPECT_TRUE(c.floored);
        EXPECT_EQ(c.limit, ES::Limit::None);
        EXPECT_EQ(c.mode, ES::Mode::Auto);
        EXPECT_EQ(ES::Scaled(SharpCrops[0], c.factor), (ES::Size{1666, 1798})); // the r1 run
    }
    // eye_size=panel without a known panel (the mock) is auto: floored too
    const ES::Choice m = ES::Choose(ES::Mode::Panel, 0.0f, SharpCrops, LinkRecommended,
                                    ES::Size{}, LinkSwapchain, LinkMaxRect, ES::DefaultMinFactor);
    EXPECT_FLOAT_EQ(m.factor, 1.13f);
    // a recommendation a little above the crop (x1.05): raised to the floor as well
    const ES::Choice n = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops,
                                    {{{1548, 1672}, {1548, 1672}}}, Quest3Panel, LinkSwapchain,
                                    LinkMaxRect, ES::DefaultMinFactor);
    EXPECT_FLOAT_EQ(n.factor, 1.13f);
    // ... and one above the floor (Link 3200x1728: 1664x1800) is the recommendation, as before
    const ES::Choice r = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops,
                                    {{{1664, 1800}, {1664, 1800}}}, Quest3Panel, LinkSwapchain,
                                    {3328, 3600}, ES::DefaultMinFactor);
    EXPECT_NEAR(r.factor, 1800.0 / 1592.0, 1e-4);
    EXPECT_FALSE(r.floored);
}

TEST(EyeScale, FloorOfOneNeverRaises) {
    // min_factor 1 (vr.ini eye_size_min_factor=1): the old choice for any crop and target
    for (u32 w = 600; w <= 2400; w += 37) {
        const u32 h = w * 1274 / 1178;
        const std::array<ES::Size, 2> crops{{{w, h}, {w + 2, h}}};
        for (const ES::Size rec :
             {ES::Size{1424, 1568}, ES::Size{2064, 2208}, ES::Size{997, 1098}}) {
            for (const ES::Size panel : {Quest3Panel, ES::Size{}}) {
                const ES::Choice b =
                    ES::Choose(ES::Mode::Auto, 0.0f, crops, {{rec, rec}}, panel, LinkSwapchain,
                               {2 * rec.width, 2 * rec.height}, 1.0f);
                EXPECT_FALSE(b.floored);
                EXPECT_FLOAT_EQ(b.factor, ES::Factor(0.0f, crops, {{rec, rec}}, LinkSwapchain,
                                                     {2 * rec.width, 2 * rec.height}));
                // the floor raises only, never lowers
                const ES::Choice f =
                    ES::Choose(ES::Mode::Auto, 0.0f, crops, {{rec, rec}}, panel, LinkSwapchain,
                               {2 * rec.width, 2 * rec.height}, ES::DefaultMinFactor);
                EXPECT_GE(f.factor, b.factor);
            }
        }
    }
}

TEST(EyeScale, FloorNeverAboveThePanelOrTheMax) {
    // a crop just under the Quest 3 panel: raised only to the panel (2064 / 1900 = 1.086)
    const std::array<ES::Size, 2> near_panel{{{1900, 2000}, {1900, 2000}}};
    const ES::Choice c = ES::Choose(ES::Mode::Auto, 0.0f, near_panel, LinkRecommended,
                                    Quest3Panel, Big, Big, ES::DefaultMinFactor);
    EXPECT_NEAR(c.factor, 2064.0 / 1900.0, 1e-4);
    EXPECT_TRUE(c.floored);
    const ES::Size s = ES::Scaled(near_panel[0], c.factor);
    EXPECT_LE(s.width, 2064u + 1u);
    EXPECT_LE(s.height, 2208u);
    // a crop already panel-sized: as rendered, as before (the compositor scales it down)
    const std::array<ES::Size, 2> big{{{2100, 2250}, {2100, 2250}}};
    const ES::Choice d = ES::Choose(ES::Mode::Auto, 0.0f, big, LinkRecommended, Quest3Panel,
                                    Big, Big, ES::DefaultMinFactor);
    EXPECT_FLOAT_EQ(d.factor, 1.0f);
    EXPECT_FALSE(d.floored);
    // the runtime's largest image rect and swapchain still cap it
    const ES::Choice e = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops, LinkRecommended,
                                    Quest3Panel, Big, {1600, 1800}, ES::DefaultMinFactor);
    EXPECT_NEAR(e.factor, 1600.0 / 1474.0, 1e-4);
    EXPECT_EQ(e.limit, ES::Limit::ImageRect);
    EXPECT_LE(ES::Scaled(SharpCrops[0], e.factor).width, 1600u);
    const ES::Choice f = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops, LinkRecommended,
                                    Quest3Panel, {3200, 4096}, Big, ES::DefaultMinFactor);
    EXPECT_NEAR(f.factor, (3200.0 - 64.0) / (2 * 1474.0), 1e-4); // 1.064
    EXPECT_EQ(f.limit, ES::Limit::Swapchain);
    // capped below 1.02: as rendered, not "raised"
    const ES::Choice g = ES::Choose(ES::Mode::Auto, 0.0f, SharpCrops, LinkRecommended,
                                    Quest3Panel, Big, {1490, 1800}, ES::DefaultMinFactor);
    EXPECT_FLOAT_EQ(g.factor, 1.0f);
    EXPECT_FALSE(g.floored);
    // a huge setting stays within auto's x2; the explicit factor and panel modes ignore it
    EXPECT_FLOAT_EQ(ES::Choose(ES::Mode::Auto, 0.0f, {{{600, 650}, {600, 650}}},
                               {{{500, 500}, {500, 500}}}, ES::Size{}, Big, Big, 9.0f)
                        .factor,
                    ES::MaxAutoFactor);
    EXPECT_FLOAT_EQ(ES::Choose(ES::Mode::Factor, 1.0f, SharpCrops, LinkRecommended, Quest3Panel,
                               Big, Big, ES::DefaultMinFactor)
                        .factor,
                    1.0f);
    const ES::Choice p = ES::Choose(ES::Mode::Panel, 0.0f, SharpCrops, LinkRecommended,
                                    Quest3Panel, LinkSwapchain, LinkMaxRect, ES::DefaultMinFactor);
    EXPECT_NEAR(p.factor, 4032.0 / 2948.0, 1e-4); // as PanelForTheOtherPresets
    EXPECT_FALSE(p.floored);
}

} // namespace
