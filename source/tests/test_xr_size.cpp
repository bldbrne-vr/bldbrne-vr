// SPDX-License-Identifier: GPL-2.0-or-later

// The headset swapchains' sizes against the runtime's limits (renderer_vulkan/vr_xr_size.h).
// The limits are the Quest 3 on Meta Link (the user's log, 2026-10-07: recommended 1424x1568,
// largest eye image rect 2848x3136, largest swapchain 4096x4096) and the mock runtime
// (recommended 2064x2208, 4096x4096 for both). The renders are the presets: tilt 2720x1808
// ("2560"), tilt 3200x2128, 4:3 2560x1920, 4:3 3200x2400 and 3456x2592; at the render vfov the
// game uses without the pitch bias (122 deg) and with it (116 deg).

#include <array>
#include <cmath>
#include <ostream>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_eye_scale.h"
#include "video_core/renderer_vulkan/vr_xr_size.h"

namespace Vulkan::VrXrSize {
inline std::ostream& operator<<(std::ostream& os, const Size& s) {
    return os << s.width << "x" << s.height;
}
} // namespace Vulkan::VrXrSize

namespace {

namespace XS = Vulkan::VrXrSize;
namespace ES = Vulkan::VrEyeScale;

struct Limits {
    const char* name;
    XS::Size recommended;
    XS::Size max_rect;
    XS::Size max_swapchain;
};
constexpr Limits Link{"Meta Link 1x", {1424, 1568}, {2848, 3136}, {4096, 4096}};
constexpr Limits Mock{"mock", {2064, 2208}, {4096, 4096}, {4096, 4096}};
constexpr std::array<Limits, 2> AllLimits{Link, Mock};

constexpr std::array<XS::Size, 5> Renders{{
    {2720, 1808}, // bb-vr-tilt-2560.xml
    {3200, 2128}, // bb-vr-tilt-3200.xml (the user's HEADSET TEST 2 SHARP)
    {2560, 1920}, // bb-vr-43.xml
    {3200, 2400}, // bb-vr-43-3200.xml
    {3456, 2592}, // bb-vr-43-3456.xml
}};
constexpr std::array<float, 2> VfovsDeg{122.0f, 116.0f};

// One Quest 3 eye's crop of a symmetric render (the left eye: L-54 R40 U44 D-55 deg; the right eye
// mirrors it, same size), as CropToEyes covers the located FOV on a fixed-FOV runtime: whole
// pixels outward.
XS::Size QuestCrop(XS::Size render, float vfov_deg) {
    constexpr double D = 3.14159265358979323846 / 180.0;
    const double tan_v = std::tan(0.5 * vfov_deg * D);
    const double tan_h = tan_v * render.width / render.height;
    const double x0 = render.width * (0.5 + 0.5 * std::tan(-54.0 * D) / tan_h);
    const double x1 = render.width * (0.5 + 0.5 * std::tan(40.0 * D) / tan_h);
    const double y0 = render.height * (0.5 - 0.5 * std::tan(44.0 * D) / tan_v);
    const double y1 = render.height * (0.5 - 0.5 * std::tan(-55.0 * D) / tan_v);
    return {static_cast<u32>(std::ceil(x1) - std::floor(x0)),
            static_cast<u32>(std::ceil(y1) - std::floor(y0))};
}

// The rule before 2026-10-07 (EnsureSwapchains): the whole render fitted into ONE eye's largest
// image rect.
XS::Size OldCentreSwapchain(XS::Size frame, XS::Size max_rect) {
    if (frame.width <= max_rect.width && frame.height <= max_rect.height) {
        return frame;
    }
    const float scale = std::min(static_cast<float>(max_rect.width) / frame.width,
                                 static_cast<float>(max_rect.height) / frame.height);
    return {static_cast<u32>(frame.width * scale), static_cast<u32>(frame.height * scale)};
}

TEST(XrSize, QuestCropMatchesTheUsersLog) {
    // The user's 2720x1808 log at vfov 122: crop 1109x1200 (CropToEyes rounds the pair together,
    // this helper outward: within a pixel or two).
    const XS::Size crop = QuestCrop({2720, 1808}, 122.0f);
    EXPECT_NEAR(static_cast<double>(crop.width), 1109.0, 2.0);
    EXPECT_NEAR(static_cast<double>(crop.height), 1200.0, 2.0);
}

TEST(XrSize, TheOldRuleScaledThe3200RendersOnLink) {
    // The bug: the 3200x2128 render's centre swapchains were 2848x1893 on Link (the log's
    // "scaling 3200x2128 frames to 2848x1893"), not the render's size, and DIBR refused them.
    EXPECT_EQ(OldCentreSwapchain({3200, 2128}, Link.max_rect), (XS::Size{2848, 1893}));
    EXPECT_EQ(OldCentreSwapchain({3200, 2400}, Link.max_rect), (XS::Size{2848, 2136}));
    // ... while the mock's 4096 max rect hid it.
    EXPECT_EQ(OldCentreSwapchain({3200, 2128}, Mock.max_rect), (XS::Size{3200, 2128}));
}

TEST(XrSize, CentreSwapchainIsTheRenderWhenItFitsTheLargestSwapchain) {
    for (const Limits& limits : AllLimits) {
        for (const XS::Size& render : Renders) {
            const XS::FrameFit fit = XS::CentreSwapchain(render, limits.max_swapchain);
            EXPECT_FALSE(fit.scaled) << limits.name << " " << render;
            EXPECT_EQ(fit.size, render) << limits.name;
        }
    }
}

TEST(XrSize, CentreSwapchainScalesARenderPastTheLargestSwapchain) {
    const XS::FrameFit wide = XS::CentreSwapchain({4800, 3200}, {4096, 4096});
    EXPECT_TRUE(wide.scaled);
    EXPECT_EQ(wide.size.width, 4096u);
    EXPECT_NEAR(static_cast<double>(wide.size.height), 3200.0 * 4096.0 / 4800.0, 1.0);
    const XS::FrameFit tall = XS::CentreSwapchain({3000, 5000}, {4096, 4096});
    EXPECT_TRUE(tall.scaled);
    EXPECT_EQ(tall.size.height, 4096u);
    EXPECT_LE(tall.size.width, 4096u);
    EXPECT_NEAR(static_cast<double>(tall.size.width), 3000.0 * 4096.0 / 5000.0, 1.0);
    // Never 0, never past the limit.
    const XS::FrameFit thin = XS::CentreSwapchain({100000, 1}, {4096, 4096});
    EXPECT_EQ(thin.size, (XS::Size{4096, 1}));
    EXPECT_FALSE(XS::CentreSwapchain({0, 0}, {4096, 4096}).scaled);
}

TEST(XrSize, EveryPresetsEyesFitBothRuntimes) {
    for (const Limits& limits : AllLimits) {
        for (const XS::Size& render : Renders) {
            for (const float vfov : VfovsDeg) {
                const XS::Size crop = QuestCrop(render, vfov);
                EXPECT_EQ(XS::CheckDibrEyes({crop, crop}, limits.max_swapchain, limits.max_rect),
                          XS::EyeRefusal::None)
                    << limits.name << " " << render << " vfov " << vfov << " crop " << crop;
                // The mono path's crops of the (unscaled) centre image fit too.
                EXPECT_TRUE(XS::CropFitsRect(crop, limits.max_rect)) << limits.name << render;
            }
        }
    }
}

TEST(XrSize, EyeSizeRuleOutputFitsTheRuntime) {
    // vr.ini eye_size=auto with eye_size_min_factor 1.13 (the default), the Quest 3 panel known:
    // the enlarged eyes still fit each runtime's limits, and the late warp's side-by-side
    // swapchain (both eyes + 64) its largest swapchain.
    const ES::Size panel{2064, 2208};
    for (const Limits& limits : AllLimits) {
        for (const XS::Size& render : Renders) {
            for (const float vfov : VfovsDeg) {
                const XS::Size c = QuestCrop(render, vfov);
                const ES::Size crop{c.width, c.height};
                const ES::Size rec{limits.recommended.width, limits.recommended.height};
                const ES::Choice choice = ES::Choose(
                    ES::Mode::Auto, 0.0f, {crop, crop}, {rec, rec}, panel,
                    {limits.max_swapchain.width, limits.max_swapchain.height},
                    {limits.max_rect.width, limits.max_rect.height}, ES::DefaultMinFactor);
                const ES::Size out = ES::Scaled(crop, choice.factor);
                EXPECT_GE(choice.factor, 1.0f);
                EXPECT_EQ(XS::CheckDibrEyes({{{out.width, out.height}, {out.width, out.height}}},
                                            limits.max_swapchain, limits.max_rect),
                          XS::EyeRefusal::None)
                    << limits.name << " " << render << " vfov " << vfov;
                EXPECT_LE(2 * out.width + 64, limits.max_swapchain.width) << limits.name;
            }
        }
    }
}

TEST(XrSize, EyeSizeRuleAtTheRealLinkRecommendation) {
    // 3200x2128 with the bias (vfov 116): a crop of about 1473x1592, already about Link's
    // recommended 1424x1568, so auto alone would be x1.0; the 1.13 floor applies (the panel allows
    // up to x1.39).
    const XS::Size c = QuestCrop({3200, 2128}, 116.0f);
    EXPECT_NEAR(static_cast<double>(c.width), 1474.0, 3.0);
    EXPECT_NEAR(static_cast<double>(c.height), 1593.0, 3.0);
    const ES::Size crop{c.width, c.height};
    const ES::Choice choice =
        ES::Choose(ES::Mode::Auto, 0.0f, {crop, crop}, {{{1424, 1568}, {1424, 1568}}},
                   {2064, 2208}, {4096, 4096}, {2848, 3136}, ES::DefaultMinFactor);
    EXPECT_TRUE(choice.floored);
    EXPECT_NEAR(choice.factor, ES::DefaultMinFactor, 1e-4);
    // 2720x1808 at vfov 122 (the user's healthy 2560 log): x1.307 of 1109x1200, 1450x1568.
    const ES::Size small{1109, 1200};
    const ES::Choice healthy =
        ES::Choose(ES::Mode::Auto, 0.0f, {small, small}, {{{1424, 1568}, {1424, 1568}}},
                   {2064, 2208}, {4096, 4096}, {2848, 3136}, ES::DefaultMinFactor);
    EXPECT_NEAR(healthy.factor, 1568.0 / 1200.0, 1e-3);
    EXPECT_FALSE(healthy.floored);
}

TEST(XrSize, CheckDibrEyesRefusals) {
    EXPECT_EQ(XS::CheckDibrEyes({{{0, 100}, {100, 100}}}, Link.max_swapchain, Link.max_rect),
              XS::EyeRefusal::Empty);
    // Wider than Link's largest eye rect (2848), within its largest swapchain.
    EXPECT_EQ(XS::CheckDibrEyes({{{3000, 1500}, {3000, 1500}}}, Link.max_swapchain, Link.max_rect),
              XS::EyeRefusal::ImageRect);
    // Taller than the largest swapchain (a runtime with a generous rect limit).
    EXPECT_EQ(XS::CheckDibrEyes({{{1000, 5000}, {1000, 1000}}}, {4096, 4096}, {8192, 8192}),
              XS::EyeRefusal::Swapchain);
    // Exactly at the limits.
    EXPECT_EQ(XS::CheckDibrEyes({{{2848, 3136}, {2848, 3136}}}, Link.max_swapchain, Link.max_rect),
              XS::EyeRefusal::None);
    EXPECT_FALSE(XS::RefusalText(XS::EyeRefusal::ImageRect).empty());
}

TEST(XrSize, DibrAllocRoundsUpWithinTheLargestSwapchain) {
    EXPECT_EQ(XS::DibrAlloc({1474, 1593}, {4096, 4096}), (XS::Size{1536, 1600}));
    EXPECT_EQ(XS::DibrAlloc({1536, 1600}, {4096, 4096}), (XS::Size{1536, 1600}));
    // Near the limit: rounded only up to it.
    EXPECT_EQ(XS::DibrAlloc({4090, 3000}, {4096, 4096}), (XS::Size{4096, 3008}));
    EXPECT_EQ(XS::DibrAlloc({2000, 2000}, {2010, 2010}), (XS::Size{2010, 2010}));
    // Never smaller than the eyes.
    for (u32 w = 1; w < 300; w += 7) {
        const XS::Size a = XS::DibrAlloc({w, w + 1}, {4096, 4096});
        EXPECT_GE(a.width, w);
        EXPECT_GE(a.height, w + 1);
        EXPECT_EQ(a.width % 64, 0u);
    }
}

} // namespace
