// SPDX-License-Identifier: GPL-2.0-or-later

// The UI layer's layout (src/core/vr/ui_layout.h): the HUD band's segments, the dialog crops and
// the band's lazy follow.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

#include <gtest/gtest.h>

#include "core/vr/ui_layout.h"
#include "core/vr/ui_rest_curve.h"
#include "core/vr/vr_session.h"

namespace {

using namespace VR::Ui;

constexpr float Deg = 0.0174532925f;

bool Overlap(const StageRect& a, const StageRect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

// The angular extents of two segments (centre +- half size) overlap.
bool AnglesOverlap(const Segment& a, const Segment& b) {
    const auto range = [](float centre, float size) {
        return std::pair{centre - 0.5f * size, centre + 0.5f * size};
    };
    const auto [ay0, ay1] = range(a.yaw_deg, a.WidthDeg());
    const auto [by0, by1] = range(b.yaw_deg, b.WidthDeg());
    const auto [ap0, ap1] = range(a.row_pitch_deg, a.HeightDeg());
    const auto [bp0, bp1] = range(b.row_pitch_deg, b.HeightDeg());
    return ay0 < by1 && by0 < ay1 && ap0 < bp1 && bp0 < ap1;
}

// Where a stage point of a segment lands on the band (deg, + = right / up).
std::pair<float, float> Angles(const Segment& seg, float x, float y) {
    const float cx = 0.5f * (seg.rect.x0 + seg.rect.x1);
    const float cy = 0.5f * (seg.rect.y0 + seg.rect.y1);
    return {seg.yaw_deg + (x - cx) * seg.deg_per_px, seg.row_pitch_deg - (y - cy) * seg.deg_per_px};
}

TEST(UiLayout, BeltSegmentsDoNotOverlap) {
    for (const float scale : {0.025f, 0.035f, 0.05f}) {
        const auto hud = ComputeHudLayout(HudLayout::Belt, scale, 0.0f);
        ASSERT_EQ(hud.count, 3u);
        for (u32 i = 0; i < hud.count; ++i) {
            for (u32 j = i + 1; j < hud.count; ++j) {
                EXPECT_FALSE(Overlap(hud.segment[i].rect, hud.segment[j].rect)) << i << " " << j;
                EXPECT_FALSE(AnglesOverlap(hud.segment[i], hud.segment[j]))
                    << i << " " << j << " at " << scale;
            }
        }
    }
}

// Bloodborne's HUD as measured in stage px (HP / items, echoes / insight, an action prompt): each
// lands within the band's dead zones, so reading it never moves the band, and near the centre.
TEST(UiLayout, BeltKeepsTheHudNearTheCentre) {
    const auto hud = ComputeHudLayout(HudLayout::Belt, 0.035f, 0.0f);
    const FollowParams follow{};
    const auto& left = hud.segment[0];
    const auto& right = hud.segment[1];
    const auto& lower = hud.segment[2];
    // HP bar / items: x 106..377, y 63..219.
    const auto hp0 = Angles(left, 106.0f, 63.0f);
    const auto hp1 = Angles(left, 377.0f, 219.0f);
    EXPECT_GE(hp0.first, -21.0f);
    EXPECT_LE(hp1.first, -8.0f);
    EXPECT_LT(std::abs(hp0.first), follow.yaw_dead_deg);
    // Echoes / insight: x 1549..1808, y 68..160.
    const auto echo0 = Angles(right, 1549.0f, 68.0f);
    const auto echo1 = Angles(right, 1808.0f, 160.0f);
    EXPECT_GE(echo0.first, 8.0f);
    EXPECT_LE(echo1.first, 21.0f);
    EXPECT_LT(std::abs(echo1.first), follow.yaw_dead_deg);
    // Rows: HP between -18 and -25 deg, within the downward dead zone.
    EXPECT_LE(hp0.second, -18.0f);
    EXPECT_GE(hp1.second, -25.0f);
    EXPECT_LT(std::abs(hp1.second), follow.pitch_dead_down_deg);
    EXPECT_LT(std::abs(echo1.second), follow.pitch_dead_down_deg);
    // An action prompt: x 491..1416, y 868..946, about 12 deg below and yaw-centred.
    const auto prompt0 = Angles(lower, 491.0f, 868.0f);
    const auto prompt1 = Angles(lower, 1416.0f, 946.0f);
    EXPECT_NEAR(0.5f * (prompt0.first + prompt1.first), 0.0f, 1.0f);
    EXPECT_NEAR(0.5f * (prompt0.second + prompt1.second), -12.0f, 1.5f);
    EXPECT_LT(std::abs(prompt1.second), follow.pitch_dead_down_deg);
    // A long HP bar (to stage x 960) still ends left of the echoes.
    EXPECT_LT(Angles(left, 960.0f, 100.0f).first, echo0.first);
}

TEST(UiLayout, BeltStaysInView) {
    const auto hud = ComputeHudLayout(HudLayout::Belt, 0.035f, 0.0f);
    for (u32 i = 0; i < hud.count; ++i) {
        const auto& seg = hud.segment[i];
        // Within +-27 deg (the binocular overlap), below the horizon, above -26 deg.
        EXPECT_LE(std::abs(seg.yaw_deg) + 0.5f * seg.WidthDeg(), 27.0f) << seg.name;
        EXPECT_LT(seg.row_pitch_deg + 0.5f * seg.HeightDeg(), 0.0f) << seg.name;
        EXPECT_GT(seg.row_pitch_deg - 0.5f * seg.HeightDeg(), -26.0f) << seg.name;
        // Readable: body text at least 0.8 deg; the lower group (prompts) larger.
        EXPECT_GE(seg.EmDeg(), 0.8f) << seg.name;
        // Inside the stage.
        EXPECT_GE(seg.rect.x0, 0.0f);
        EXPECT_LE(seg.rect.x1, StageWidth);
        EXPECT_GE(seg.rect.y0, 0.0f);
        EXPECT_LE(seg.rect.y1, StageHeight);
    }
    EXPECT_GE(hud.segment[2].EmDeg(), 0.95f);
    // The status clusters are the ones shown with dialogs; the lower group is split by content.
    EXPECT_TRUE(hud.segment[0].with_dialogs);
    EXPECT_TRUE(hud.segment[1].with_dialogs);
    EXPECT_FALSE(hud.segment[2].with_dialogs);
    EXPECT_FALSE(hud.segment[0].dynamic);
    EXPECT_TRUE(hud.segment[2].dynamic);
}

TEST(UiLayout, BeltPitchOffsetMovesEveryRow) {
    const auto a = ComputeHudLayout(HudLayout::Belt, 0.035f, 0.0f);
    const auto b = ComputeHudLayout(HudLayout::Belt, 0.035f, 5.0f);
    for (u32 i = 0; i < a.count; ++i) {
        EXPECT_NEAR(b.segment[i].row_pitch_deg - a.segment[i].row_pitch_deg, 5.0f, 1e-5f);
        EXPECT_NEAR(b.segment[i].yaw_deg, a.segment[i].yaw_deg, 1e-5f);
    }
}

TEST(UiLayout, GameLayoutHasNoBand) {
    EXPECT_EQ(ComputeHudLayout(HudLayout::Game, 0.035f, 0.0f).count, 0u);
}

TEST(UiLayout, DialogCropsAreCentredAndSized) {
    for (const bool note : {false, true}) {
        const StageRect r = DialogCrop(note);
        EXPECT_NEAR(0.5f * (r.x0 + r.x1), StageWidth * 0.5f, 1e-3f);
        EXPECT_NEAR(0.5f * (r.y0 + r.y1), StageHeight * 0.5f, 1e-3f);
        EXPECT_GE(r.Width(), 985.0f); // the OK box is ~985 stage px wide
        // The canvas makes its body text em_deg tall.
        const float width = CanvasWidthDeg(r, true, 80.0f, 1.3f);
        EXPECT_NEAR(width / r.Width() * BodyTextPx, 1.3f, 1e-4f);
        // Inside the region the dialog box is measured in.
        EXPECT_GE(r.x0, DialogBoxRegion.x0);
        EXPECT_LE(r.x1, DialogBoxRegion.x1);
    }
    // A full menu: the menu width, text = width / 80 deg.
    const StageRect full{0.0f, 0.0f, StageWidth, StageHeight};
    EXPECT_NEAR(CanvasWidthDeg(full, false, 80.0f, 1.3f) / StageWidth * BodyTextPx, 1.0f, 1e-4f);
    EXPECT_NEAR(BandScale(2560.0f), 4.0f / 3.0f, 1e-6f);
}

TEST(UiLayout, DialogCropLocksOnAStillBox) {
    CropState st{};
    const std::array<s32, 4> box{400, 500, 1700, 800};
    const StageRect fallback = DialogCrop(false);
    // Three frames with the same box (still for two): locked around it with the margin.
    EXPECT_FALSE(StepDialogCrop(st, true, true, box, 32.0f, 0.016f, fallback, 4.0f / 3.0f));
    EXPECT_FALSE(StepDialogCrop(st, true, true, box, 32.0f, 0.016f, fallback, 4.0f / 3.0f));
    EXPECT_TRUE(StepDialogCrop(st, true, true, box, 32.0f, 0.016f, fallback, 4.0f / 3.0f));
    EXPECT_TRUE(st.valid);
    EXPECT_NEAR(st.cx, 1050.0f, 1e-3f);
    EXPECT_NEAR(st.cy, 650.0f, 1e-3f);
    EXPECT_NEAR(st.hw, 682.0f, 1e-3f);
    EXPECT_NEAR(st.hh, 182.0f, 1e-3f);
    // More lines: it grows symmetrically, its centre never moves.
    const std::array<s32, 4> taller{400, 470, 1700, 800};
    EXPECT_TRUE(StepDialogCrop(st, true, true, taller, 32.0f, 0.016f, fallback, 4.0f / 3.0f));
    EXPECT_NEAR(st.cy, 650.0f, 1e-3f);
    EXPECT_NEAR(st.hh, 212.0f, 1e-3f);
    // A smaller box (a line faded out) never shrinks it.
    EXPECT_FALSE(StepDialogCrop(st, true, true, box, 32.0f, 0.016f, fallback, 4.0f / 3.0f));
    EXPECT_NEAR(st.hh, 212.0f, 1e-3f);
}

TEST(UiLayout, DialogCropNeverStaysHidden) {
    // A box that never holds still (an animated element in the region): locked after the timeout
    // on the newest box.
    CropState st{};
    for (int i = 0; i < 30 && !st.valid; ++i) {
        const std::array<s32, 4> box{400 + 7 * i, 500, 1700, 800};
        StepDialogCrop(st, true, true, box, 32.0f, 0.033f, DialogCrop(false), 4.0f / 3.0f);
    }
    EXPECT_TRUE(st.valid);
    EXPECT_LE(st.waited_s, 0.45f);
    // No box at all (none of the dialog's own frames measured one): the centred fallback.
    CropState none{};
    for (int i = 0; i < 30 && !none.valid; ++i) {
        StepDialogCrop(none, false, false, {}, 32.0f, 0.033f, DialogCrop(true), 4.0f / 3.0f);
    }
    EXPECT_TRUE(none.valid);
    EXPECT_NEAR(none.cx, 1280.0f, 1e-2f);
    EXPECT_NEAR(none.cy, 720.0f, 1e-2f);
    EXPECT_NEAR(2.0f * none.hw, 1160.0f * 4.0f / 3.0f, 1e-2f);
}

FollowParams Params() {
    return FollowParams{};
}

TEST(UiLayout, FollowPlacesOnFirstStep) {
    FollowState st{};
    EXPECT_TRUE(StepHudFollow(st, 0.3f, -0.1f, {1.0f, 1.6f, 2.0f}, 0.011f, Params()));
    EXPECT_TRUE(st.valid);
    EXPECT_NEAR(st.yaw, 0.3f, 1e-6f);
    EXPECT_NEAR(st.pitch, -0.1f, 1e-6f);
    EXPECT_NEAR(st.origin.x, 1.0f, 1e-6f);
}

TEST(UiLayout, FollowIdlesInsideTheDeadZone) {
    FollowState st{};
    StepHudFollow(st, 0.0f, 0.0f, {}, 0.011f, Params());
    for (int i = 0; i < 200; ++i) {
        StepHudFollow(st, 25.0f * Deg, 10.0f * Deg, {}, 0.011f, Params());
    }
    EXPECT_NEAR(st.yaw, 0.0f, 1e-6f);
    EXPECT_NEAR(st.pitch, 0.0f, 1e-6f);
    // Looking down at the band's lowest row never moves it.
    for (int i = 0; i < 200; ++i) {
        StepHudFollow(st, 0.0f, -26.0f * Deg, {}, 0.011f, Params());
    }
    EXPECT_NEAR(st.pitch, 0.0f, 1e-6f);
}

TEST(UiLayout, FollowChasesBeyondTheDeadZoneAndSettles) {
    FollowState st{};
    StepHudFollow(st, 0.0f, 0.0f, {}, 0.011f, Params());
    for (int i = 0; i < 300; ++i) { // 3.3 s, 11 time constants
        StepHudFollow(st, 40.0f * Deg, 0.0f, {}, 0.011f, Params());
    }
    // Chased until within settle_deg (3 deg), then idle.
    EXPECT_LE(std::abs(40.0f * Deg - st.yaw), 3.0f * Deg + 1e-4f);
    EXPECT_FALSE(st.chase_yaw);
    const float settled = st.yaw;
    for (int i = 0; i < 100; ++i) {
        StepHudFollow(st, 40.0f * Deg, 0.0f, {}, 0.011f, Params());
    }
    EXPECT_NEAR(st.yaw, settled, 1e-6f);
    // Looking up at a tall boss (+40 deg) brings the band up.
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, 40.0f * Deg, 40.0f * Deg, {}, 0.011f, Params());
    }
    EXPECT_LE(std::abs(40.0f * Deg - st.pitch), 3.0f * Deg + 1e-4f);
    // Looking at the floor far below the band brings it down.
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, 40.0f * Deg, -20.0f * Deg, {}, 0.011f, Params());
    }
    EXPECT_LE(std::abs(-20.0f * Deg - st.pitch), 3.0f * Deg + 1e-4f);
}

TEST(UiLayout, FollowNeverLagsMoreThanTheHardLimit) {
    FollowState st{};
    StepHudFollow(st, 0.0f, 0.0f, {}, 0.011f, Params());
    // A fast turn: 90 deg in one step.
    StepHudFollow(st, 90.0f * Deg, 0.0f, {}, 0.011f, Params());
    EXPECT_LE(std::abs(90.0f * Deg - st.yaw), 45.0f * Deg + 1e-4f);
}

TEST(UiLayout, FollowWrapsAroundPi) {
    FollowState st{};
    StepHudFollow(st, 170.0f * Deg, 0.0f, {}, 0.011f, Params());
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, -172.0f * Deg, 0.0f, {}, 0.011f, Params()); // 18 deg away, the short way
    }
    // Within the dead zone the short way round: it never chased through 0.
    EXPECT_NEAR(std::abs(WrapRad(st.yaw - 170.0f * Deg)), 0.0f, 1e-5f);
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, -150.0f * Deg, 0.0f, {}, 0.011f, Params()); // 40 deg the short way
    }
    EXPECT_LE(std::abs(WrapRad(-150.0f * Deg - st.yaw)), 3.0f * Deg + 1e-4f);
}

TEST(UiLayout, FollowClampsPitch) {
    FollowState st{};
    StepHudFollow(st, 0.0f, -80.0f * Deg, {}, 0.011f, Params());
    EXPECT_NEAR(st.pitch, -50.0f * Deg, 1e-5f);
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, 0.0f, 70.0f * Deg, {}, 0.011f, Params());
    }
    EXPECT_LE(st.pitch, 50.0f * Deg + 1e-5f);
}

TEST(UiLayout, FollowInvalidationPlacesAgain) {
    FollowState st{};
    StepHudFollow(st, 0.0f, 0.0f, {}, 0.011f, Params());
    st.valid = false; // a recenter
    StepHudFollow(st, 1.0f, 0.2f, {0.5f, 1.5f, 0.0f}, 0.011f, Params());
    EXPECT_NEAR(st.yaw, 1.0f, 1e-6f);
    EXPECT_NEAR(st.pitch, 0.2f, 1e-6f);
    EXPECT_NEAR(st.origin.x, 0.5f, 1e-6f);
}

TEST(UiLayout, FollowSmoothsTheOrigin) {
    FollowState st{};
    StepHudFollow(st, 0.0f, 0.0f, {0.0f, 1.6f, 0.0f}, 0.011f, Params());
    StepHudFollow(st, 0.0f, 0.0f, {0.2f, 1.6f, 0.0f}, 0.011f, Params());
    EXPECT_GT(st.origin.x, 0.0f);
    EXPECT_LT(st.origin.x, 0.05f); // one 11 ms step of a 200 ms time constant
    for (int i = 0; i < 300; ++i) {
        StepHudFollow(st, 0.0f, 0.0f, {0.2f, 1.6f, 0.0f}, 0.011f, Params());
    }
    EXPECT_NEAR(st.origin.x, 0.2f, 1e-3f);
}

// The HUD fixed to the view: Bloodborne's HUD content (stage px) lands in the view's lower
// corners, the HP bar's left end and the echoes' right end at the corner yaw, body text 1 deg.
TEST(UiLayout, ViewAnchorPutsTheClustersInTheLowerCorners) {
    const auto hud = ComputeHudLayout(HudLayout::Belt, 0.042f, 0.0f, HudAnchor::View);
    ASSERT_EQ(hud.count, 3u);
    const auto& left = hud.segment[0];
    const auto& right = hud.segment[1];
    const auto& lower = hud.segment[2];
    for (u32 i = 0; i < hud.count; ++i) {
        EXPECT_NEAR(hud.segment[i].EmDeg(), 1.008f, 1e-3f) << hud.segment[i].name;
    }
    // HP bar / items (x 106..466 with the item's name, y 62..219): from the corner inward.
    const auto hp0 = Angles(left, HudLeftContentX, 62.0f);
    const auto items1 = Angles(left, 466.0f, 219.0f);
    EXPECT_NEAR(hp0.first, -ViewCornerYawDeg, 1e-3f);
    EXPECT_NEAR(items1.first, -14.9f, 0.1f);
    EXPECT_NEAR(hp0.second, -17.9f, 0.1f);
    EXPECT_NEAR(items1.second, -24.5f, 0.1f);
    // Echoes / insight (x 1549..1808, y 68..160): from the corner inward.
    const auto echo0 = Angles(right, 1549.0f, 68.0f);
    const auto echo1 = Angles(right, HudRightContentX, 160.0f);
    EXPECT_NEAR(echo1.first, ViewCornerYawDeg, 1e-3f);
    EXPECT_NEAR(echo0.first, 19.1f, 0.1f);
    EXPECT_GT(echo0.second, -19.0f);
    EXPECT_LT(echo1.second, -21.0f);
    // A long HP bar (to stage x 960) still ends left of the echoes.
    EXPECT_LT(Angles(left, 960.0f, 70.0f).first, echo0.first);
    // An action prompt (x 491..1416, y 868..946): centred, 9 to 12.5 deg below the view's centre.
    const auto prompt0 = Angles(lower, 491.0f, 868.0f);
    const auto prompt1 = Angles(lower, 1416.0f, 946.0f);
    EXPECT_NEAR(0.5f * (prompt0.first + prompt1.first), 0.0f, 1.0f);
    EXPECT_NEAR(prompt0.second, -9.2f, 0.1f);
    EXPECT_NEAR(prompt1.second, -12.5f, 0.1f);
    // Subtitles and a boss bar (down to stage y 1010) stay above the clusters' top row.
    EXPECT_GT(Angles(lower, 960.0f, 1010.0f).second, hp0.second + 2.0f);
    // Every corner of the HUD's content within the Quest 3's binocular view (+-40 deg) and below
    // the horizon.
    for (const auto& a : {hp0, items1, echo0, echo1, prompt0, prompt1}) {
        EXPECT_LE(std::abs(a.first), 40.0f);
        EXPECT_LT(a.second, 0.0f);
        EXPECT_GT(a.second, -30.0f);
    }
}

TEST(UiLayout, ViewAnchorCornersFollowTheSettings) {
    const auto wide = ComputeHudLayout(HudLayout::Belt, 0.042f, 0.0f, HudAnchor::View, 34.0f,
                                       -21.0f);
    EXPECT_NEAR(Angles(wide.segment[0], HudLeftContentX, 100.0f).first, -34.0f, 1e-3f);
    EXPECT_NEAR(Angles(wide.segment[1], HudRightContentX, 100.0f).first, 34.0f, 1e-3f);
    // Up in the top corners as on the flat screen; the lower group stays.
    const auto top = ComputeHudLayout(HudLayout::Belt, 0.042f, 0.0f, HudAnchor::View, 30.0f, 18.0f);
    EXPECT_NEAR(top.segment[0].row_pitch_deg, 18.0f, 1e-5f);
    EXPECT_NEAR(top.segment[1].row_pitch_deg, 18.0f, 1e-5f);
    EXPECT_NEAR(top.segment[2].row_pitch_deg, ViewLowerPitchDeg, 1e-5f);
    // ui_hud_pitch_deg moves every row; the size scales the HUD about its corners.
    const auto up = ComputeHudLayout(HudLayout::Belt, 0.042f, 3.0f, HudAnchor::View);
    const auto base = ComputeHudLayout(HudLayout::Belt, 0.042f, 0.0f, HudAnchor::View);
    for (u32 i = 0; i < base.count; ++i) {
        EXPECT_NEAR(up.segment[i].row_pitch_deg - base.segment[i].row_pitch_deg, 3.0f, 1e-5f);
    }
    const auto big = ComputeHudLayout(HudLayout::Belt, 0.05f, 0.0f, HudAnchor::View);
    EXPECT_NEAR(Angles(big.segment[0], HudLeftContentX, 100.0f).first, -ViewCornerYawDeg, 1e-3f);
    // The band anchor is the old layout, unchanged.
    const auto band = ComputeHudLayout(HudLayout::Belt, 0.035f, 0.0f);
    const auto band2 = ComputeHudLayout(HudLayout::Belt, 0.035f, 0.0f, HudAnchor::Band, 40.0f, 9.0f);
    for (u32 i = 0; i < band.count; ++i) {
        EXPECT_EQ(band.segment[i].yaw_deg, band2.segment[i].yaw_deg);
        EXPECT_EQ(band.segment[i].row_pitch_deg, band2.segment[i].row_pitch_deg);
    }
}

// A full menu cropped to its content: as large as fits, never smaller than the whole menu.
TEST(UiLayout, MenuFitEnlargesMostlyEmptyMenus) {
    const StageRect full{0.0f, 0.0f, StageWidth, StageHeight};
    // The whole menu: today's 72 deg canvas exactly.
    EXPECT_NEAR(MenuDegPerPx(full, 72.0f, 44.0f, 1.3f, 72.0f) * StageWidth, 72.0f, 1e-3f);
    // The headstone's travel list (its panel, x 400..1545, y 105..1000, plus the margin): height
    // bound, 44 deg tall, its text a quarter larger.
    const StageRect travel{376.0f, 81.0f, 1569.0f, 1024.0f};
    const float t = MenuDegPerPx(travel, 72.0f, 44.0f, 1.3f, 72.0f);
    EXPECT_NEAR(travel.Height() * t, 44.0f, 1e-3f);
    EXPECT_LE(travel.Width() * t, 72.0f);
    EXPECT_GT(BodyTextPx * t, 1.1f);
    // The Options grid (x 280..960, y 400..880): text capped at the dialogs' 1.3 deg.
    const StageRect grid{256.0f, 376.0f, 984.0f, 904.0f};
    EXPECT_NEAR(BodyTextPx * MenuDegPerPx(grid, 72.0f, 44.0f, 1.3f, 72.0f), 1.3f, 1e-4f);
    // The Inventory (nearly the whole screen) under a 66 deg width: today's size, its content's
    // corners where they were (the floor: the whole menu at 72 deg).
    const StageRect inventory{75.0f, 20.0f, 1843.0f, 1042.0f};
    const float i = MenuDegPerPx(inventory, 66.0f, 44.0f, 1.3f, 72.0f);
    EXPECT_NEAR(i, 72.0f / StageWidth, 1e-7f);
    EXPECT_LE(inventory.Height() * i, 44.0f);
    // A list panel 1300 px wide: width bound at 66 deg.
    const StageRect list{310.0f, 140.0f, 1610.0f, 940.0f};
    EXPECT_NEAR(list.Width() * MenuDegPerPx(list, 66.0f, 44.0f, 1.3f, 72.0f), 66.0f, 1e-3f);
    // Never smaller than the whole menu, even with a low height limit.
    for (const auto& r : {full, travel, grid, inventory}) {
        EXPECT_GE(MenuDegPerPx(r, 72.0f, 30.0f, 1.3f, 72.0f), 72.0f / StageWidth - 1e-7f);
    }
}

// The view angles (+ = right / up, the atan of the view-plane tangent) of a point on a canvas
// piece: a cylinder segment `width_deg` wide round the origin at `radius`, its centre `yaw_deg`
// right and `pitch_deg` up (its axis tilted with the pitch, as an XR cylinder layer posed by
// QuatFromYawPitch), the point `a_deg` across its arc from the centre and `h_rad` (metres per
// radius) up along its axis.
ViewAngles PieceView(float yaw_deg, float pitch_deg, float a_deg, float h_rad) {
    const float a = a_deg * Deg;
    const float p = pitch_deg * Deg;
    const float t = yaw_deg * Deg;
    const float x = std::sin(a);
    const float y = h_rad;
    const float z = -std::cos(a);
    const float y1 = y * std::cos(p) - z * std::sin(p);
    const float z1 = y * std::sin(p) + z * std::cos(p);
    const float x2 = x * std::cos(t) - z1 * std::sin(t);
    const float z2 = x * std::sin(t) + z1 * std::cos(t);
    return {std::atan(x2 / -z2) / Deg, std::atan(y1 / -z2) / Deg};
}

TEST(UiLayout, CanvasCornersAreViewAngles) {
    // A flat quad: across exactly half its width, up its tangent ratio.
    const auto flat = CanvasCorner(60.0f, 33.75f, false);
    EXPECT_NEAR(flat.yaw_deg, 30.0f, 1e-3f);
    EXPECT_NEAR(std::tan(flat.pitch_deg * Deg), std::tan(30.0f * Deg) * 33.75f / 60.0f, 1e-4f);
    // A cylinder: its corners are higher than its middle column's top, as PieceView finds them.
    for (const float w : {30.0f, 60.0f, 68.0f, 80.0f}) {
        for (const float h : {20.0f, 38.0f, 50.0f}) {
            const auto c = CanvasCorner(w, h, true);
            const auto v = PieceView(0.0f, 0.0f, 0.5f * w, 0.5f * h * Deg);
            EXPECT_NEAR(c.yaw_deg, v.yaw_deg, 1e-3f) << w << " " << h;
            EXPECT_NEAR(c.pitch_deg, v.pitch_deg, 1e-3f) << w << " " << h;
            EXPECT_GT(c.pitch_deg, std::atan(0.5f * h * Deg) / Deg) << w << " " << h;
        }
    }
    // The fit: the corners on the limit that binds, never past either.
    for (const bool curved : {true, false}) {
        for (const auto& [w, h] : {std::pair{1920.0f, 1080.0f}, std::pair{1193.0f, 943.0f},
                                   std::pair{600.0f, 1000.0f}, std::pair{1118.0f, 286.0f}}) {
            const float dpp = SharedViewDegPerPx(w, h, curved);
            const auto c = CanvasCorner(w * dpp, h * dpp, curved);
            EXPECT_LE(c.yaw_deg, CornerYawDefaultDeg + 1e-3f) << w << " " << h;
            EXPECT_LE(c.pitch_deg, CornerPitchDefaultDeg + 1e-3f) << w << " " << h;
            EXPECT_TRUE(std::abs(c.yaw_deg - CornerYawDefaultDeg) < 0.01f ||
                        std::abs(c.pitch_deg - CornerPitchDefaultDeg) < 0.01f)
                << w << " " << h << " " << c.yaw_deg << " " << c.pitch_deg;
        }
    }
}

// Every menu, dialog and note seen in the mock runs (their measured crops, stage px with the
// margin) inside the view both eyes share, with the largest text that fits.
TEST(UiLayout, EveryCanvasFitsTheSharedView) {
    const VR::Settings d{};
    CanvasLimits l{};
    l.max_width_deg = d.ui_menu_max_width_deg;
    l.max_height_deg = d.ui_menu_max_height_deg;
    l.max_em_deg = d.ui_dialog_em_deg;
    l.dialog_max_width_deg = d.ui_dialog_max_deg;
    l.full_width_deg = d.ui_menu_width_deg;
    l.corner_yaw_deg = d.ui_menu_corner_yaw_deg;
    l.corner_pitch_deg = d.ui_menu_corner_pitch_deg;
    struct Case {
        const char* name;
        StageRect rect;
        CanvasKind kind;
        float text_min; // deg
    };
    // The dialogs: at most ui_dialog_max_deg (34) across, so the game's ~1000 px boxes get
    // 0.70-0.80 deg text (DialogsHaveAComfortableSize).
    const Case cases[] = {
        {"inventory", {75.0f, 20.0f, 1843.0f, 1042.0f}, CanvasKind::Menu, 0.92f},
        {"status (whole band)", {0.0f, 0.0f, StageWidth, StageHeight}, CanvasKind::Menu, 0.85f},
        {"options grid", {262.0f, 395.0f, 990.0f, 895.0f}, CanvasKind::Menu, 1.3f},
        {"travel list", {376.0f, 81.0f, 1569.0f, 1024.0f}, CanvasKind::Menu, 1.3f},
        {"item list", {262.0f, 129.0f, 1503.0f, 944.0f}, CanvasKind::Menu, 1.29f},
        {"equipment box", {411.0f, 223.0f, 1503.0f, 842.0f}, CanvasKind::Menu, 1.3f},
        {"yes/no box", {397.0f, 343.0f, 1515.0f, 629.0f}, CanvasKind::Dialog, 0.72f},
        {"talk choices", {455.0f, 695.0f, 1471.0f, 1024.0f}, CanvasKind::Dialog, 0.80f},
        {"note (fallback)", DialogCrop(true), CanvasKind::Dialog, 0.70f},
        {"dialog (fallback)", DialogCrop(false), CanvasKind::Dialog, 0.72f},
        {"a wide dialog", {100.0f, 400.0f, 1820.0f, 700.0f}, CanvasKind::Dialog, 0.70f},
        {"whole menu", {0.0f, 0.0f, StageWidth, StageHeight}, CanvasKind::Whole, 0.85f},
    };
    for (const bool curved : {true, false}) {
        l.curved = curved;
        for (const Case& c : cases) {
            const float dpp = CanvasDegPerPx(c.rect, c.kind, l);
            const float w = c.rect.Width() * dpp;
            const float h = c.rect.Height() * dpp;
            const auto corner = CanvasCorner(w, h, curved);
            std::printf("[ %s %s ] %.1f x %.1f deg, text %.2f deg, corners +-%.1f / +-%.1f deg\n",
                        c.name, curved ? "curved" : "flat", w, h, BodyTextPx * dpp,
                        corner.yaw_deg, corner.pitch_deg);
            EXPECT_LE(corner.yaw_deg, 34.0f + 1e-3f) << c.name;
            EXPECT_LE(corner.pitch_deg, 30.0f + 1e-3f) << c.name;
            EXPECT_LE(corner.yaw_deg, SharedViewHalfDeg - 5.0f) << c.name;
            EXPECT_GE(BodyTextPx * dpp,
                      curved ? c.text_min - 0.005f : std::min(0.8f, c.text_min) - 0.005f)
                << c.name;
            EXPECT_LE(BodyTextPx * dpp, d.ui_dialog_em_deg + 1e-4f) << c.name;
            // The largest that fits: the text at its cap, a corner on its limit, or a dialog at
            // its width cap (or its text floor).
            const bool dialog_capped =
                c.kind == CanvasKind::Dialog &&
                (w > d.ui_dialog_max_deg - 0.01f || BodyTextPx * dpp < DialogMinEmDeg + 1e-3f);
            EXPECT_TRUE(BodyTextPx * dpp > d.ui_dialog_em_deg - 1e-3f ||
                        corner.yaw_deg > 34.0f - 0.01f || corner.pitch_deg > 30.0f - 0.01f ||
                        dialog_capped)
                << c.name;
            if (c.kind == CanvasKind::Dialog && BodyTextPx * dpp > DialogMinEmDeg + 1e-3f) {
                EXPECT_LE(w, d.ui_dialog_max_deg + 1e-3f) << c.name;
            }
        }
    }
}

// Small dialogs (ui_dialog_max_deg): the playtest's crops (2026-10-08, stage px with the margin)
// came out 55-68 deg across at the dialogs' text size alone ("the tooltip text ... looked
// ENORMOUS"); now at most 34 deg across, their text near the menus' and below the HUD's.
TEST(UiLayout, DialogsHaveAComfortableSize) {
    const VR::Settings d{};
    EXPECT_FLOAT_EQ(d.ui_dialog_max_deg, DialogMaxDegDefault);
    EXPECT_FLOAT_EQ(DialogMaxDegDefault, 34.0f);
    CanvasLimits l{};
    l.max_em_deg = d.ui_dialog_em_deg;
    l.dialog_max_width_deg = d.ui_dialog_max_deg;
    l.full_width_deg = d.ui_menu_width_deg;
    l.corner_yaw_deg = d.ui_menu_corner_yaw_deg;
    l.corner_pitch_deg = d.ui_menu_corner_pitch_deg;
    CanvasLimits before = l;
    before.dialog_max_width_deg = 0.0f; // the old size: ui_dialog_em_deg alone
    const auto at = [](float cx, float cy, float w, float h) {
        return StageRect{cx - 0.5f * w, cy - 0.5f * h, cx + 0.5f * w, cy + 0.5f * h};
    };
    struct Case {
        const char* name;
        StageRect rect;
        float before_w, before_text; // deg (the playtest log's numbers)
        float w, text;               // deg
    };
    const Case cases[] = {
        // IngameMessageBox, lower centre (the pickup pop-up's style): "dialog crop locked around
        // stage (962, 859), 1017 x 329 stage px = 55.1 x 17.8 deg (text 1.30 deg)".
        {"object message", at(962.0f, 859.0f, 1017.0f, 329.0f), 55.1f, 1.30f, 34.0f, 0.802f},
        // MessageBox, centre ("Return to the Hunter's Dream? YES / NO"): 60.6 x 15.5 deg.
        {"yes/no box", at(956.0f, 487.0f, 1118.0f, 286.0f), 60.6f, 1.30f, 34.0f, 0.730f},
        // The same message measured with a dim across the screen: 68.0 x 18.7 deg (the corners'
        // cap), text 1.10; now the text floor wins over the width cap.
        {"message over a dim", at(960.0f, 899.0f, 1488.0f, 409.0f), 68.0f, 1.10f, 43.4f, 0.70f},
    };
    for (const Case& c : cases) {
        const float b = CanvasDegPerPx(c.rect, CanvasKind::Dialog, before);
        EXPECT_NEAR(c.rect.Width() * b, c.before_w, 0.06f) << c.name;
        EXPECT_NEAR(BodyTextPx * b, c.before_text, 0.006f) << c.name;
        const float a = CanvasDegPerPx(c.rect, CanvasKind::Dialog, l);
        const float w = c.rect.Width() * a;
        std::printf("[ %s ] %.1f x %.1f deg (text %.2f) -> %.1f x %.1f deg (text %.2f)\n", c.name,
                    c.rect.Width() * b, c.rect.Height() * b, BodyTextPx * b, w,
                    c.rect.Height() * a, BodyTextPx * a);
        EXPECT_NEAR(w, c.w, 0.06f) << c.name;
        EXPECT_NEAR(BodyTextPx * a, c.text, 0.003f) << c.name;
        // Smaller than the gameplay HUD's text (1.0 deg) and the full menus' (0.85-0.92).
        EXPECT_LT(BodyTextPx * a, d.ui_hud_scale_deg * BodyTextPx + 1e-3f) << c.name;
        EXPECT_LE(BodyTextPx * a, 0.85f) << c.name;
        EXPECT_GE(BodyTextPx * a, DialogMinEmDeg - 1e-4f) << c.name;
    }
    // A narrow box keeps the dialogs' text size (the cap does not bind): 400 px = 21.7 deg.
    const StageRect narrow = at(960.0f, 540.0f, 400.0f, 200.0f);
    EXPECT_NEAR(BodyTextPx * CanvasDegPerPx(narrow, CanvasKind::Dialog, l), d.ui_dialog_em_deg,
                1e-4f);
    // Wider crops: never larger text; never wider than the cap unless the text is at its floor.
    float last_text = 10.0f;
    for (float w_px = 200.0f; w_px <= 1900.0f; w_px += 25.0f) {
        const float dpp = DialogDegPerPx(w_px, d.ui_dialog_em_deg, d.ui_dialog_max_deg);
        const float text = BodyTextPx * dpp;
        EXPECT_LE(text, last_text + 1e-5f) << w_px;
        EXPECT_LE(text, d.ui_dialog_em_deg + 1e-5f) << w_px;
        EXPECT_GE(text, DialogMinEmDeg - 1e-5f) << w_px;
        if (text > DialogMinEmDeg + 1e-4f) {
            EXPECT_LE(w_px * dpp, d.ui_dialog_max_deg + 1e-3f) << w_px;
        }
        last_text = text;
    }
    // The setting: a tighter cap shrinks the box (down to the text floor); 0 = no cap.
    EXPECT_NEAR(1017.0f * DialogDegPerPx(1017.0f, 1.3f, 30.0f), 30.0f, 1e-3f);
    EXPECT_NEAR(BodyTextPx * DialogDegPerPx(1017.0f, 1.3f, 20.0f), DialogMinEmDeg, 1e-4f);
    EXPECT_NEAR(BodyTextPx * DialogDegPerPx(1017.0f, 1.3f, 0.0f), 1.3f, 1e-4f);
    // A smaller ui_dialog_em_deg than the floor is kept (the floor never enlarges the text).
    EXPECT_NEAR(BodyTextPx * DialogDegPerPx(1488.0f, 0.6f, 34.0f), 0.6f, 1e-4f);
}

// The view-fixed HUD (the default anchor and size): every corner of its pieces (their whole
// rects, empty margins included) inside the shared view, within +-34 / +-30 deg.
TEST(UiLayout, ViewAnchorHudCornersFitTheSharedView) {
    const VR::Settings d{};
    const auto hud = ComputeHudLayout(HudLayout::Belt, d.ui_hud_scale_deg, d.ui_hud_pitch_deg,
                                      HudAnchor::View, d.ui_hud_corner_yaw_deg,
                                      d.ui_hud_corner_pitch_deg);
    ASSERT_EQ(hud.count, 3u);
    for (u32 i = 0; i < hud.count; ++i) {
        const auto& seg = hud.segment[i];
        float max_yaw = 0.0f, max_pitch = 0.0f;
        for (const float sx : {-0.5f, 0.5f}) {
            for (const float sy : {-0.5f, 0.5f}) {
                const auto v = PieceView(seg.yaw_deg, seg.row_pitch_deg, sx * seg.WidthDeg(),
                                         sy * seg.HeightDeg() * Deg);
                max_yaw = std::max(max_yaw, std::abs(v.yaw_deg));
                max_pitch = std::max(max_pitch, std::abs(v.pitch_deg));
            }
        }
        std::printf("[ HUD %s ] corners up to %.1f deg across, %.1f deg up/down\n", seg.name,
                    max_yaw, max_pitch);
        EXPECT_LE(max_yaw, 34.0f) << seg.name;
        EXPECT_LE(max_pitch, 30.0f) << seg.name;
    }
}

// The HUD under a full menu's canvas (ui_menu_hud, MenuHudLayout): below the canvas's lowest
// point, its outer content edges under the canvas's edges (or where the gameplay HUD's are, for
// a narrow menu), inside the view both eyes share across, the two clusters apart.
TEST(UiLayout, MenuHudSitsUnderTheCanvas) {
    const VR::Settings d{};
    struct Canvas {
        const char* name;
        float w, h; // deg, as BuildUiLayers shows them (stage px x deg per px)
    };
    // Measured canvases (documents/bloodborne-vr.md): the Options grid, the Doll's and the
    // Messengers' command list, the Inventory / shop, Status, the travel list.
    const std::array<Canvas, 5> canvases{{{"grid", 39.4f, 27.1f},
                                          {"command list", 41.5f, 30.4f},
                                          {"inventory", 68.0f, 39.3f},
                                          {"status", 68.0f, 38.2f},
                                          {"travel list", 63.0f, 50.7f}}};
    for (const bool curved : {true, false}) {
        for (const auto& c : canvases) {
            const auto hud = MenuHudLayout(c.w, c.h, curved, d.ui_hud_scale_deg,
                                           d.ui_hud_corner_yaw_deg, d.ui_hud_corner_pitch_deg,
                                           d.ui_menu_corner_yaw_deg);
            ASSERT_EQ(hud.count, 2u) << c.name;
            const float s = d.ui_hud_scale_deg;
            const float bottom = CanvasCorner(c.w, c.h, curved).pitch_deg;
            const auto& left = hud.segment[0];
            const auto& right = hud.segment[1];
            EXPECT_STREQ(left.name, "status_left");
            EXPECT_STREQ(right.name, "status_right");
            // A point of a segment (stage x, y) as a view angle from the canvas's origin.
            const auto at = [&](const Segment& seg, float x, float y) {
                const float a = (x - 0.5f * (seg.rect.x0 + seg.rect.x1)) * s;
                const float h = (0.5f * (seg.rect.y0 + seg.rect.y1) - y) * s * Deg;
                return PieceView(seg.yaw_deg, seg.row_pitch_deg, a, h);
            };
            // The HP bar's top and the echoes' top: below the canvas's corners, with the gap.
            const float hp_top = at(left, HudLeftContentX, HudContentTopY).pitch_deg;
            const float echo_top = at(right, HudRightContentX, HudContentTopY).pitch_deg;
            std::printf("[ %s %s ] canvas bottom -%.1f, HP top %.1f, echoes top %.1f, HP left "
                        "end %.1f, echoes right end %.1f\n",
                        c.name, curved ? "curved" : "flat", bottom, hp_top, echo_top,
                        at(left, HudLeftContentX, 135.0f).yaw_deg,
                        at(right, HudRightContentX, 135.0f).yaw_deg);
            EXPECT_LE(hp_top, -bottom - 0.5f * MenuHudGapDeg) << c.name;
            EXPECT_LE(echo_top, -bottom - 0.5f * MenuHudGapDeg) << c.name;
            // Never higher than the gameplay HUD's rows (a small menu: where the HUD is).
            EXPECT_LE(left.row_pitch_deg, d.ui_hud_corner_pitch_deg + 1e-3f) << c.name;
            // Across: the content's outer edges inside the shared view (+-34 deg as seen from the
            // canvas's origin, also far below its centre), the HP bar's left end under the
            // canvas's left edge, the clusters apart; the helper agrees with PieceView.
            float reach = 0.0f;
            for (const float y : {HudContentTopY, HudContentBottomY}) {
                const float l = at(left, HudLeftContentX, y).yaw_deg;
                const float r = at(right, HudRightContentX, y).yaw_deg;
                EXPECT_NEAR(l, SegmentPointView(left, HudLeftContentX, y).yaw_deg, 0.01f);
                EXPECT_NEAR(r, SegmentPointView(right, HudRightContentX, y).yaw_deg, 0.01f);
                EXPECT_GE(l, -d.ui_menu_corner_yaw_deg - 0.05f) << c.name;
                EXPECT_LE(r, d.ui_menu_corner_yaw_deg + 0.05f) << c.name;
                reach = std::max({reach, -l, r});
            }
            float rect_reach = 0.0f; // the rects' empty margins too (information)
            for (const float y : {left.rect.y0, left.rect.y1}) {
                rect_reach = std::max({rect_reach, -at(left, left.rect.x0, y).yaw_deg,
                                       at(right, right.rect.x1, y).yaw_deg});
            }
            std::printf("[ %s %s ] content out to +-%.1f deg (rects +-%.1f)\n", c.name,
                        curved ? "curved" : "flat", reach, rect_reach);
            EXPECT_GE(reach, std::min(0.5f * c.w, d.ui_menu_corner_yaw_deg) - 0.1f) << c.name;
            EXPECT_LT(at(left, left.rect.x1, 135.0f).yaw_deg,
                      at(right, right.rect.x0, 135.0f).yaw_deg)
                << c.name;
            // The body text at the gameplay HUD's size.
            EXPECT_NEAR(left.EmDeg(), d.ui_hud_scale_deg * BodyTextPx, 1e-4f);
        }
    }
    // A user who wants the HUD further out (ui_hud_corner_yaw_deg 40) gets it, beyond the canvas.
    const auto wide = MenuHudLayout(39.4f, 27.1f, true, d.ui_hud_scale_deg, 40.0f,
                                    d.ui_hud_corner_pitch_deg, d.ui_menu_corner_yaw_deg);
    EXPECT_NEAR(std::max(-SegmentPointView(wide.segment[0], HudLeftContentX, HudContentTopY)
                              .yaw_deg,
                         -SegmentPointView(wide.segment[0], HudLeftContentX, HudContentBottomY)
                              .yaw_deg),
                40.0f, 0.1f);
}

// ui_menu_fit_screen_wide=0: a placed crop never grows to a screen-wide dim (the shop's quantity
// box over its list, a fade, an NPC's subtitle band as the talk list closes); it still locks on
// one before it is placed, and a real sub-page (a margin at the sides) still grows it.
TEST(UiLayout, MenuCropKeepsSizeUnderScreenWideDim) {
    const float w = 1920.0f, h = 1080.0f;
    const std::array<s32, 4> shop{99, 44, 1819, 1018};  // the Messengers' list (mock 2026-10-07)
    const std::array<s32, 4> dim{0, 0, 1920, 1080};     // the quantity box's dim
    const std::array<s32, 4> band{12, 292, 1908, 1080}; // the Doll's subtitle band, closing
    EXPECT_FALSE(IsScreenWideBox(shop, w));
    EXPECT_TRUE(IsScreenWideBox(dim, w));
    EXPECT_TRUE(IsScreenWideBox(band, w));
    EXPECT_FALSE(IsScreenWideBox({0, 0, 1700, 1080}, w)); // one side only
    MenuCropState st{};
    // Not placed yet: every box counts (a menu that is all dim still locks).
    EXPECT_TRUE(MenuCropTakesBox(st, true, dim, w, false));
    for (int n = 0; n < 3; ++n) {
        StepMenuCrop(st, MenuCropTakesBox(st, true, shop, w, false), true, shop, 24.0f, w, h,
                     0.016f);
    }
    ASSERT_TRUE(st.valid);
    const MenuCropState placed = st;
    // The quantity box for 2 s, then the list again: the crop never moves.
    for (int n = 0; n < 125; ++n) {
        const bool takes = MenuCropTakesBox(st, true, dim, w, false);
        EXPECT_FALSE(takes);
        EXPECT_FALSE(StepMenuCrop(st, takes, true, dim, 24.0f, w, h, 0.016f));
    }
    for (int n = 0; n < 60; ++n) {
        EXPECT_FALSE(StepMenuCrop(st, MenuCropTakesBox(st, true, shop, w, false), true, shop, 24.0f,
                                  w, h, 0.016f));
    }
    EXPECT_EQ(st.x0, placed.x0);
    EXPECT_EQ(st.y0, placed.y0);
    EXPECT_EQ(st.x1, placed.x1);
    EXPECT_EQ(st.y1, placed.y1);
    EXPECT_FALSE(MenuCropTakesBox(st, true, band, w, false));
    // A box with no measurement is passed on as before (StepMenuCrop ignores it).
    EXPECT_TRUE(MenuCropTakesBox(st, false, dim, w, false));
    // ui_menu_fit_screen_wide=1: the old way, it grows to the whole band.
    EXPECT_TRUE(MenuCropTakesBox(st, true, dim, w, true));
    EXPECT_TRUE(StepMenuCrop(st, true, true, dim, 24.0f, w, h, 0.016f));
    EXPECT_EQ(st.x0, 0.0f);
    EXPECT_EQ(st.x1, w);
}

// The canvas opens where you look, kept within the menu panel's pitch range, plus the menu's own
// offset (the travel list ui_travel_pitch_deg: lower).
TEST(UiLayout, CanvasAimPitchWithMenuOffset) {
    EXPECT_FLOAT_EQ(CanvasAimPitchDeg(0.0f, 0.0f, -30.0f, 15.0f), 0.0f);
    EXPECT_FLOAT_EQ(CanvasAimPitchDeg(5.0f, -10.0f, -30.0f, 15.0f), -5.0f);
    EXPECT_FLOAT_EQ(CanvasAimPitchDeg(40.0f, 0.0f, -30.0f, 15.0f), 15.0f);
    EXPECT_FLOAT_EQ(CanvasAimPitchDeg(40.0f, -10.0f, -30.0f, 15.0f), 5.0f);
    EXPECT_FLOAT_EQ(CanvasAimPitchDeg(-50.0f, -10.0f, -30.0f, 15.0f), -40.0f);
    // The travel list (62.8 x 50.5 deg, entries 17-20 deg above its centre): with the default
    // -10 they come 7-10 deg above where you look, its key guide 29-31 deg below.
    const float centre = CanvasAimPitchDeg(0.0f, -10.0f, -30.0f, 15.0f);
    EXPECT_NEAR(centre + 18.5f, 8.5f, 1.6f);
    EXPECT_NEAR(centre - 20.0f, -30.0f, 1.1f);
}

TEST(UiLayout, MenuCropLocksGrowsAndShrinks) {
    MenuCropState st{};
    const float w = 2560.0f, h = 1440.0f;
    const std::array<s32, 4> grid{373, 560, 1280, 1173};
    // Three frames with the same box: locked around it with the margin.
    EXPECT_FALSE(StepMenuCrop(st, true, true, grid, 32.0f, w, h, 0.016f));
    EXPECT_FALSE(StepMenuCrop(st, true, true, grid, 32.0f, w, h, 0.016f));
    EXPECT_TRUE(StepMenuCrop(st, true, true, grid, 32.0f, w, h, 0.016f));
    EXPECT_TRUE(st.valid);
    EXPECT_NEAR(st.x0, 341.0f, 1e-3f);
    EXPECT_NEAR(st.y1, 1205.0f, 1e-3f);
    // A sub-page over more of the screen: it grows at once to hold both.
    const std::array<s32, 4> inventory{100, 27, 2457, 1389};
    EXPECT_TRUE(StepMenuCrop(st, true, true, inventory, 32.0f, w, h, 0.016f));
    EXPECT_NEAR(st.x0, 68.0f, 1e-3f);
    EXPECT_NEAR(st.y0, 0.0f, 1e-3f); // clamped to the band
    EXPECT_NEAR(st.x1, 2489.0f, 1e-3f);
    // Back to the grid: smaller, so after 0.6 s it shrinks to it (not before).
    int steps = 0;
    while (!StepMenuCrop(st, true, true, grid, 32.0f, w, h, 0.016f) && steps < 100) {
        ++steps;
    }
    EXPECT_GE(steps * 0.016f, 0.55f);
    EXPECT_LE(steps * 0.016f, 0.65f);
    EXPECT_NEAR(st.x0, 341.0f, 1e-3f);
    EXPECT_NEAR(st.x1, 1312.0f, 1e-3f);
    // A blinking element that, with the content, is not much smaller: no shrink, nothing cut.
    MenuCropState blink{};
    const std::array<s32, 4> list{500, 100, 2000, 1300};
    const std::array<s32, 4> list_less{500, 100, 2000, 1200};
    for (int n = 0; n < 3; ++n) {
        StepMenuCrop(blink, true, true, list, 32.0f, w, h, 0.016f);
    }
    ASSERT_TRUE(blink.valid);
    for (int n = 0; n < 100; ++n) {
        EXPECT_FALSE(StepMenuCrop(blink, true, true, n % 2 ? list : list_less, 32.0f, w, h,
                                  0.016f));
    }
    EXPECT_NEAR(blink.y1, 1332.0f, 1e-3f);
    // Never a box (the menu's frames draw only the HUD): the whole menu after the timeout.
    MenuCropState none{};
    for (int n = 0; n < 30 && !none.valid; ++n) {
        StepMenuCrop(none, false, false, {}, 32.0f, w, h, 0.033f);
    }
    EXPECT_TRUE(none.valid);
    EXPECT_NEAR(none.x1 - none.x0, w, 1e-3f);
    EXPECT_NEAR(none.y1 - none.y0, h, 1e-3f);
}

// Status opened from the Options grid (mock menufix/runs/t1, the band 2720 x 1530): the game cross-
// fades the grid out (screen-wide boxes, 1-8% of the band opaque) and the book in (65%, 83%, then
// 77% opaque, its opaque UI over the whole band). It is a page, not a dim: the crop grows to it
// (it was kept at the grid's, Status cut down to 39 x 27 degrees), on the book's second frame
// (the page check takes two in a row; the growth then needs no more). B back: the grid slides in
// (x0 450 -> 468 over six frames) and, with ui_menu_snap, the crop the menu locked on (the grid's)
// is taken on its first frame, not 0.6 s later; the slide plays inside it.
TEST(UiLayout, StatusPageOverTheGridGrowsTheCrop) {
    const float w = 2720.0f, h = 1530.0f;
    const float band = w * h;
    const auto px = [&](float share) { return static_cast<u32>(share * band); };
    const std::array<s32, 4> grid{405, 594, 1369, 1234};
    const std::array<s32, 4> whole{0, 0, 2720, 1530};
    MenuCropState st{};
    PageRun run{};
    for (int n = 0; n < 3; ++n) {
        StepMenuCrop(st, true, true, grid, 34.0f, w, h, 0.033f);
    }
    ASSERT_TRUE(st.valid);
    const MenuCropState placed = st;
    // One frame of the menu: the page check, then the crop.
    const auto frame = [&](const std::array<s32, 4>& box, const std::array<s32, 4>& opaque_box,
                           float opaque, bool page_setting = true, float quick = 0.5f) {
        const bool wide = IsScreenWideBox(box, w);
        const bool page_now = wide && IsScreenWidePage(st, opaque_box, px(opaque), w, h);
        const bool page = StepScreenWidePage(run, true, page_now) && page_setting;
        const bool takes = MenuCropTakesBox(st, true, box, w, false, page);
        if (!takes) {
            st.grow_seen = 0;
        }
        return StepMenuCrop(st, takes, true, box, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f, quick,
                            &opaque_box, page ? 1u : 2u, quick > 0.0f ? MenuGrowSlack : 0.0f);
    };
    // The cross-fade: screen-wide but hardly opaque (a dim): kept.
    EXPECT_FALSE(frame(whole, {620, 598, 1363, 1234}, 0.038f));
    EXPECT_FALSE(frame(whole, {618, 666, 1356, 1159}, 0.011f));
    EXPECT_FALSE(frame(whole, {155, 196, 2471, 1421}, 0.082f));
    EXPECT_EQ(st.x0, placed.x0);
    EXPECT_EQ(st.x1, placed.x1);
    // The book: one frame is not a page (a fade's peak never grows it); the second grows the crop
    // to the whole band at once, then it stays.
    EXPECT_FALSE(frame(whole, {0, 0, 2567, 1423}, 0.646f));
    EXPECT_EQ(st.x1, placed.x1);
    EXPECT_TRUE(frame(whole, whole, 0.826f));
    EXPECT_EQ(st.x0, 0.0f);
    EXPECT_EQ(st.y0, 0.0f);
    EXPECT_EQ(st.x1, w);
    EXPECT_EQ(st.y1, h);
    for (int n = 0; n < 60; ++n) {
        EXPECT_FALSE(frame(whole, whole, 0.772f));
    }
    EXPECT_EQ(st.x1, w);
    // B: the book fades out, a frame of the HUD only (no box), then the grid slides in.
    EXPECT_FALSE(frame(whole, {155, 194, 2565, 1423}, 0.125f));
    EXPECT_FALSE(frame(whole, {0, 0, 0, 0}, 0.0f));
    StepMenuCrop(st, false, false, {}, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f, 0.5f);
    int frames = 0;
    bool shrank = false;
    for (const s32 x0 : {450, 451, 455, 460, 464, 468, 468, 468, 468, 468}) {
        ++frames;
        shrank = frame({x0, 594, x0 + 901, 1234}, {625, 598, 1368, 1234}, 0.056f);
        if (shrank) {
            break;
        }
    }
    EXPECT_TRUE(shrank);
    EXPECT_EQ(frames, 1); // the grid's first frame: the crop it locked on, at once
    EXPECT_EQ(st.x0, placed.x0);
    EXPECT_EQ(st.y0, placed.y0);
    EXPECT_EQ(st.x1, placed.x1);
    EXPECT_EQ(st.y1, placed.y1);
    // The rest of its slide plays inside it: the crop never moves again.
    for (const s32 x0 : {451, 455, 460, 464, 468, 468, 468, 468, 468}) {
        EXPECT_FALSE(frame({x0, 594, x0 + 901, 1234}, {625, 598, 1368, 1234}, 0.056f));
    }
    EXPECT_EQ(st.x0, placed.x0);
    EXPECT_EQ(st.x1, placed.x1);
    // quick_area 0 (ui_menu_snap=0): the old 0.6 s.
    MenuCropState late = st;
    late.x0 = 0.0f;
    late.y0 = 0.0f;
    late.x1 = w;
    late.y1 = h;
    int late_frames = 0;
    while (!StepMenuCrop(late, true, true, {468, 594, 1369, 1234}, 34.0f, w, h, 0.033f, 2, 0.4f,
                         0.6f, 0.8f, 0.0f) &&
           late_frames < 100) {
        ++late_frames;
    }
    EXPECT_GE(late_frames * 0.033f, 0.55f);
    // ui_menu_page=0: the book stays cut down to the grid (the builds of 2026-10-07 to -09).
    st = placed;
    run = {};
    for (int n = 0; n < 10; ++n) {
        EXPECT_FALSE(frame(whole, whole, 0.772f, false));
    }
    EXPECT_EQ(st.x1, placed.x1);
}

// The presenter's step of a full menu's crop for one fresh frame of the Options grid's menu, with
// what the game says (`cell`: VR::GameMenuGridCell): the page checks, then the crop
// (openxr_presenter.cpp UpdateUiPlacement, ui_menu_page=1, ui_menu_snap=1).
struct GridMenu {
    MenuCropState st{};
    PageRun run{};
    u32 changes{0};
    bool Frame(s32 cell, const std::array<s32, 4>& box, const std::array<s32, 4>& opaque_box,
               float opaque) {
        const float w = 2720.0f, h = 1530.0f;
        const bool wide = IsScreenWideBox(box, w);
        const bool page_now =
            wide && IsScreenWidePage(st, opaque_box, static_cast<u32>(opaque * w * h), w, h);
        const bool grid_page = IsGridPageAtOnce(st, cell, true, box, w, h);
        const bool page = StepScreenWidePage(run, true, page_now) || grid_page;
        const bool takes = MenuCropTakesBox(st, true, box, w, false, page);
        if (!takes) {
            st.grow_seen = 0;
        }
        const auto step_box =
            takes ? GridPageGrowthBox(st, cell, box, 34.0f, MenuGrowSlack, w, h, w / 1920.0f) : box;
        const bool changed = StepMenuCrop(st, takes, true, step_box, 34.0f, w, h, 0.033f, 2, 0.4f,
                                          0.6f, 0.8f, MenuQuickShrinkArea, &opaque_box,
                                          page ? 1u : 2u, MenuGrowSlack);
        changes += changed && st.valid ? 1u : 0u;
        return changed;
    }
    // The grid opened and locked (its box and its opaque UI held still).
    void OpenGrid(s32 cell) {
        for (int n = 0; n < 4; ++n) {
            Frame(cell, {405, 594, 1369, 1234}, {625, 598, 1368, 1234}, 0.056f);
        }
        changes = 0;
    }
    [[nodiscard]] bool Whole() const {
        return st.x0 == 0.0f && st.y0 == 0.0f && st.x1 == 2720.0f && st.y1 == 1530.0f;
    }
};

// Status from the Options grid, its cross-fade as measured (mock menufix/runs/f3, the game's frames
// 4098..4106, band 2720 x 1530; runs/v1 the same): the book's backdrop over the whole band from the
// first frame, opaque UI 3.8, 1.1, 4.8, 2.0, 8.2% (the grid fading out, the book's parts), then the
// book at 64 and 77%. Known from the game (the grid, its cursor on Stats): the crop is the whole
// band after the cross-fade's first frame (the box is a frame late: from the second one on, all of
// Status at its size), one step, no move after it. On the pixels alone (any other cell, or another
// menu) it waits for the book to be opaque, as before.
TEST(UiLayout, StatusFromTheGridAtItsFirstFrame) {
    const std::array<s32, 4> whole{0, 0, 2720, 1530};
    struct Frame {
        std::array<s32, 4> opaque_box;
        float opaque;
    };
    const Frame fade[] = {
        {{620, 598, 1363, 1234}, 0.038f}, {{618, 666, 1356, 1159}, 0.011f},
        {{615, 219, 2433, 1234}, 0.048f}, {{613, 220, 2432, 1225}, 0.020f},
        {{155, 196, 2471, 1421}, 0.082f}, {{0, 0, 2567, 1423}, 0.646f},
        {{0, 0, 2565, 1423}, 0.644f},     {whole, 0.772f},
        {whole, 0.772f},                  {whole, 0.772f},
    };
    // Which of the cross-fade's frames grew the crop to the whole band (-1: none).
    const auto status_at = [&](GridMenu& g, s32 cell) {
        int at = -1;
        for (int i = 0; i < 10; ++i) {
            if (g.Frame(cell, whole, fade[i].opaque_box, fade[i].opaque) && at < 0 && g.Whole()) {
                at = i;
            }
        }
        return at;
    };
    GridMenu known{};
    known.OpenGrid(GridCellStats);
    ASSERT_TRUE(known.st.valid);
    const MenuCropState grid = known.st;
    const int known_at = status_at(known, GridCellStats);
    EXPECT_EQ(known_at, 0);         // its first frame
    EXPECT_EQ(known.changes, 1u);                   // and nothing after it
    EXPECT_TRUE(known.Whole());
    // The pixels alone: the second opaque book frame (frame 4104), 6 game frames later.
    GridMenu pixels{};
    pixels.OpenGrid(GridCellStats);
    const int pixels_at = status_at(pixels, GridNone);
    EXPECT_EQ(pixels_at, 6);
    EXPECT_EQ(pixels.changes, 1u);
    // The cursor on another cell (a cross-fade that is not Status's): the pixels' rule.
    GridMenu other{};
    other.OpenGrid(0);
    const int other_at = status_at(other, 0);
    EXPECT_EQ(other_at, 6);
    // The grid's cursor not known (vr_menu=0): at once while the crop is the one it locked on ...
    GridMenu unknown{};
    unknown.OpenGrid(GridCursorUnknown);
    const int unknown_at = status_at(unknown, GridCursorUnknown);
    EXPECT_EQ(unknown_at, 0);
    // ... not once the grid's crop moved (the pixels' rule then).
    MenuCropState moved = grid;
    moved.x1 += 40.0f;
    EXPECT_FALSE(IsGridPageAtOnce(moved, GridCursorUnknown, true, whole, 2720.0f, 1530.0f));
    EXPECT_TRUE(IsGridPageAtOnce(moved, GridCellStats, true, whole, 2720.0f, 1530.0f));
    // B back to the grid: the book fades out, then the grid's first frame takes its crop at once.
    EXPECT_FALSE(known.Frame(GridCellStats, whole, {155, 194, 2565, 1423}, 0.125f));
    EXPECT_FALSE(known.Frame(GridCellStats, whole, {0, 0, 0, 0}, 0.0f));
    EXPECT_TRUE(known.Frame(GridCellStats, {450, 598, 1351, 1234}, {}, 0.0f));
    EXPECT_EQ(known.st.x0, grid.x0);
    EXPECT_EQ(known.st.y0, grid.y0);
    EXPECT_EQ(known.st.x1, grid.x1);
    EXPECT_EQ(known.st.y1, grid.y1);
    // Only a box over the whole band, of a placed crop that is not the whole band yet, with the
    // grid known: not the Doll's subtitle band (screen-wide, not the whole height), not a box
    // before the lock, not an invalid box, not any other menu (the shop's quantity box: a dim over
    // the whole band at a menu's first crop).
    EXPECT_TRUE(IsWholeBandBox(whole, 2720.0f, 1530.0f));
    EXPECT_TRUE(IsWholeBandBox({20, 25, 2700, 1510}, 2720.0f, 1530.0f));
    EXPECT_FALSE(IsWholeBandBox({0, 456, 2719, 1530}, 2720.0f, 1530.0f));
    EXPECT_FALSE(IsWholeBandBox({147, 0, 2570, 1530}, 2720.0f, 1530.0f));
    EXPECT_FALSE(IsGridPageAtOnce(grid, GridCellStats, true, {0, 456, 2719, 1530}, 2720.0f,
                                  1530.0f));
    EXPECT_FALSE(IsGridPageAtOnce(MenuCropState{}, GridCellStats, true, whole, 2720.0f, 1530.0f));
    EXPECT_FALSE(IsGridPageAtOnce(grid, GridCellStats, false, whole, 2720.0f, 1530.0f));
    EXPECT_FALSE(IsGridPageAtOnce(grid, GridNone, true, whole, 2720.0f, 1530.0f));
    MenuCropState full = grid;
    full.x0 = 0.0f;
    full.y0 = 0.0f;
    full.x1 = 2720.0f;
    full.y1 = 1530.0f;
    EXPECT_FALSE(IsGridPageAtOnce(full, GridCellStats, true, whole, 2720.0f, 1530.0f));
}

// System from the Options grid, as measured (mock menufix/runs/v1, band 2720 x 1530): its panel
// comes in over the grid's fading frames (the boxes the union of both), the game pauses 0.23 s
// while its list loads, then the panel settles higher and wider at (619, 350)..(2092, 1155). With
// the grid's cursor on System the crop grows once, on the panel's second frame, to the crop the
// settled panel needs (stage (262, 223)..(1501, 895), as the second step reached before), and never
// moves again; on the pixels alone it grew twice (66.0 x 34.8, then 67.1 x 36.4 degrees). B back:
// the grid's first frame takes its crop at once.
TEST(UiLayout, SystemFromTheGridInOneStep) {
    const float k = 2720.0f / 1920.0f;
    struct Frame {
        std::array<s32, 4> box, opaque_box;
        float opaque;
    };
    const Frame open[] = {
        {{452, 594, 1369, 1234}, {625, 598, 1368, 1234}, 0.056f}, // the cursor on System
        {{448, 406, 2038, 1234}, {623, 605, 1357, 1157}, 0.014f}, // its first frames ...
        {{443, 392, 2064, 1234}, {629, 751, 1344, 1147}, 0.003f},
        {{443, 392, 2064, 1234}, {629, 751, 1344, 1147}, 0.003f},
        {{439, 389, 2075, 1234}, {742, 758, 1318, 1126}, 0.0f}, // ... the load pause after it
        {{619, 350, 2092, 1155}, {732, 383, 1988, 1121}, 0.002f}, // settled
        {{619, 350, 2092, 1155}, {732, 383, 1988, 1121}, 0.002f},
        {{619, 350, 2092, 1155}, {732, 383, 1988, 1121}, 0.002f},
    };
    GridMenu known{};
    known.OpenGrid(GridCellSystem);
    const MenuCropState grid = known.st;
    int at = -1;
    for (int i = 0; i < 8; ++i) {
        if (known.Frame(GridCellSystem, open[i].box, open[i].opaque_box, open[i].opaque) &&
            at < 0) {
            at = i;
        }
    }
    EXPECT_EQ(at, 2);             // the panel's second frame
    EXPECT_EQ(known.changes, 1u); // one step
    EXPECT_NEAR(known.st.x0, grid.x0, 1e-3f);
    EXPECT_NEAR(known.st.y0 / k, 223.0f, 1.0f);
    EXPECT_NEAR(known.st.x1 / k, 1501.0f, 1.0f);
    EXPECT_NEAR(known.st.y1, grid.y1, 1e-3f);
    // The pixels alone (another cell; or no cursor): two steps, the second to the same crop.
    GridMenu pixels{};
    pixels.OpenGrid(GridCursorUnknown);
    for (const auto& f : open) {
        pixels.Frame(GridCursorUnknown, f.box, f.opaque_box, f.opaque);
    }
    EXPECT_EQ(pixels.changes, 2u);
    EXPECT_NEAR(pixels.st.y0, known.st.y0, 2.0f);
    EXPECT_NEAR(pixels.st.x1, known.st.x1, 2.0f);
    // At the grid with the cursor on System, its own frames (in the crop) never grow it.
    GridMenu idle{};
    idle.OpenGrid(GridCellSystem);
    for (int n = 0; n < 30; ++n) {
        EXPECT_FALSE(idle.Frame(GridCellSystem, open[0].box, open[0].opaque_box, 0.056f));
    }
    EXPECT_EQ(idle.st.x1, grid.x1);
    EXPECT_TRUE(GridPageGrowthBox(grid, GridCellSystem, open[0].box, 34.0f, MenuGrowSlack,
                                  2720.0f, 1530.0f, k) == open[0].box);
    // Only System's cell: the Inventory's growth is its own box.
    const std::array<s32, 4> inventory{147, 62, 2570, 1442};
    EXPECT_TRUE(GridPageGrowthBox(grid, 0, inventory, 34.0f, MenuGrowSlack, 2720.0f, 1530.0f, k) ==
                inventory);
    // The panel it takes in: the settled one, in band px.
    const auto taken = GridPageGrowthBox(grid, GridCellSystem, open[1].box, 34.0f, MenuGrowSlack,
                                         2720.0f, 1530.0f, k);
    EXPECT_TRUE(taken == (std::array<s32, 4>{448, 349, 2093, 1234}));
    // B back: System's panel fades out (inside the crop), then the grid's first frame: its crop.
    for (const s32 x0 : {636, 654, 697}) {
        EXPECT_FALSE(known.Frame(GridCellSystem, {x0, 393, 2057, 1130}, {}, 0.0f));
    }
    EXPECT_TRUE(known.Frame(GridCellSystem, {434, 598, 1345, 1234}, {}, 0.0f));
    EXPECT_EQ(known.st.x0, grid.x0);
    EXPECT_EQ(known.st.y0, grid.y0);
    EXPECT_EQ(known.st.x1, grid.x1);
    EXPECT_EQ(known.st.y1, grid.y1);
}

// The page check never takes a dim over a menu: the shop's or the Discard's quantity box over
// its list (the list's own opaque panels inside the crop, the box in the middle), an NPC's
// subtitle band as the talk list closes (a line of opaque text under a small list), a sub-page's
// fade.
TEST(UiLayout, ScreenWideDimIsNotAPage) {
    const float w = 2720.0f, h = 1530.0f;
    const float band = w * h;
    const auto px = [&](float share) { return static_cast<u32>(share * band); };
    // The Inventory's crop (stage (75, 20)..(1843, 1042) at 1.4167 band px per stage px).
    MenuCropState inv{};
    inv.valid = true;
    inv.x0 = 106.0f;
    inv.y0 = 28.0f;
    inv.x1 = 2611.0f;
    inv.y1 = 1476.0f;
    // Its panels opaque (even 60% of the band) and the quantity box: all inside the crop.
    EXPECT_FALSE(IsScreenWidePage(inv, {140, 60, 2577, 1450}, px(0.60f), w, h));
    // The Doll's list (stage (589, 269)..(1355, 830)) and her subtitle line under it.
    MenuCropState list{};
    list.valid = true;
    list.x0 = 834.0f;
    list.y0 = 381.0f;
    list.x1 = 1920.0f;
    list.y1 = 1176.0f;
    EXPECT_FALSE(IsScreenWidePage(list, {600, 381, 2120, 1420}, px(0.12f), w, h));
    // A fade that is opaque over the whole band (were it not taken out as a veil) is one frame:
    // StepScreenWidePage needs PageFrames of them.
    MenuCropState grid{};
    grid.valid = true;
    grid.x0 = 371.0f;
    grid.y0 = 560.0f;
    grid.x1 = 1403.0f;
    grid.y1 = 1268.0f;
    EXPECT_TRUE(IsScreenWidePage(grid, {0, 0, 2720, 1530}, px(1.0f), w, h));
    PageRun run{};
    EXPECT_FALSE(StepScreenWidePage(run, true, true));
    EXPECT_FALSE(StepScreenWidePage(run, true, false));
    EXPECT_FALSE(StepScreenWidePage(run, true, true));
    EXPECT_FALSE(StepScreenWidePage(run, false, false)); // no new box: no change
    EXPECT_TRUE(StepScreenWidePage(run, true, true));
    // Not placed yet, or nothing opaque: never a page (MenuCropTakesBox takes every box then).
    EXPECT_FALSE(IsScreenWidePage(MenuCropState{}, {0, 0, 2720, 1530}, px(1.0f), w, h));
    EXPECT_FALSE(IsScreenWidePage(grid, {}, px(0.5f), w, h));
}

// The Options grid opening (mock menufix/runs/t2, band 2720 x 1530): the game fades it in while it
// slides it 16 band px right, and the slide paused for three frames while nothing of it was
// opaque yet. On the box alone the crop locked there (x1 1355 + margin) and then followed the
// slide in three steps; with the opaque UI's box it waits for the grid to be there and still.
TEST(UiLayout, MenuCropLocksOnceTheMenuIsInPlace) {
    const float w = 2720.0f, h = 1530.0f;
    struct Frame {
        std::array<s32, 4> box, opaque;
    };
    const std::array<s32, 4> none{};
    const Frame frames[] = {
        {{389, 598, 1345, 1234}, none},
        {{391, 594, 1355, 1234}, none},
        {{391, 594, 1355, 1234}, none},
        {{391, 594, 1355, 1234}, none}, // the pause: three equal boxes, nothing opaque
        {{396, 594, 1359, 1234}, {630, 758, 1129, 1145}},
        {{401, 594, 1364, 1234}, {623, 750, 1358, 1157}},
        {{405, 594, 1369, 1234}, {625, 598, 1368, 1234}},
        {{405, 594, 1369, 1234}, {625, 598, 1368, 1234}},
        {{405, 594, 1369, 1234}, {625, 598, 1368, 1234}},
        {{405, 594, 1369, 1234}, {625, 598, 1368, 1234}},
    };
    MenuCropState old{}, now{};
    int old_at = -1, now_at = -1, old_moves = 0;
    float old_lock_x1 = 0.0f;
    for (int i = 0; i < 10; ++i) {
        const auto& f = frames[i];
        if (StepMenuCrop(old, true, true, f.box, 34.0f, w, h, 0.033f)) {
            if (old_at < 0) {
                old_at = i;
                old_lock_x1 = old.x1;
            } else {
                ++old_moves;
            }
        }
        if (StepMenuCrop(now, true, true, f.box, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f,
                         MenuQuickShrinkArea, &f.opaque) &&
            now_at < 0) {
            now_at = i;
        }
    }
    EXPECT_EQ(old_at, 3);
    EXPECT_NEAR(old_lock_x1, 1355.0f + 34.0f, 1e-3f); // locked mid-slide ...
    EXPECT_EQ(old_moves, 3);                          // ... then grew with it, three times
    EXPECT_EQ(now_at, 8);                             // the third frame of the grid in place
    EXPECT_NEAR(now.x0, 405.0f - 34.0f, 1e-3f);
    EXPECT_NEAR(now.x1, 1369.0f + 34.0f, 1e-3f);
    // A menu with no opaque UI at all still locks, on the timeout (0.4 s) on its newest box.
    MenuCropState faint{};
    int faint_at = -1;
    for (int i = 0; i < 20 && faint_at < 0; ++i) {
        if (StepMenuCrop(faint, true, true, frames[1].box, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f,
                         MenuQuickShrinkArea, &none)) {
            faint_at = i;
        }
    }
    EXPECT_EQ(faint_at, 12); // 13 frames of 33 ms: 0.43 s
}

// ui_menu_snap: a growth needs two of the menu's frames in a row reaching out of the crop. Leave
// on the Doll's list (mock menufix/runs/d1, band 2720 x 1530): one frame of the list with her
// subtitle line under it, then her subtitle band (screen-wide, not taken), then closed: the crop
// (and the list on it) never moves. One frame of the quantity box's dim fading out, reaching the
// left edge only (runs/t3), neither. The Inventory over the grid (two frames) still grows it.
TEST(UiLayout, MenuCropGrowsOnTwoFramesInARow) {
    const float w = 2720.0f, h = 1530.0f;
    const auto step = [&](MenuCropState& st, const std::array<s32, 4>& box, bool takes = true) {
        if (!takes) {
            st.grow_seen = 0;
        }
        return StepMenuCrop(st, takes, true, box, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f,
                            MenuQuickShrinkArea, nullptr, 2);
    };
    const std::array<s32, 4> list{869, 415, 1886, 1142};
    MenuCropState doll{};
    for (int n = 0; n < 3; ++n) {
        step(doll, list);
    }
    ASSERT_TRUE(doll.valid);
    const MenuCropState placed = doll;
    EXPECT_FALSE(step(doll, {872, 450, 1883, 1411}));      // the subtitle line under the list
    EXPECT_FALSE(step(doll, {0, 456, 2719, 1530}, false)); // her subtitle band: not taken
    EXPECT_FALSE(step(doll, {872, 450, 1883, 1411}));      // again: not in a row
    EXPECT_EQ(doll.y1, placed.y1);
    // The Inventory: the dim's one-sided frame does not grow it, two frames of a sub-page do.
    const std::array<s32, 4> inventory{147, 62, 2570, 1442};
    MenuCropState inv{};
    for (int n = 0; n < 3; ++n) {
        step(inv, inventory);
    }
    ASSERT_TRUE(inv.valid);
    EXPECT_FALSE(step(inv, {3, 62, 2570, 1530}));
    EXPECT_FALSE(step(inv, inventory));
    EXPECT_NEAR(inv.x0, 147.0f - 34.0f, 1e-3f);
    MenuCropState grid{};
    for (int n = 0; n < 3; ++n) {
        step(grid, {405, 594, 1369, 1234});
    }
    EXPECT_FALSE(step(grid, {146, 63, 2570, 1442}));
    EXPECT_TRUE(step(grid, {147, 62, 2570, 1442}));
    EXPECT_NEAR(grid.x0, 146.0f - 34.0f, 1e-3f); // the union of the two
    EXPECT_NEAR(grid.y1, 1442.0f + 34.0f, 1e-3f);
    // grow_frames 1 (ui_menu_snap=0): at once, as before.
    MenuCropState once{};
    for (int n = 0; n < 3; ++n) {
        StepMenuCrop(once, true, true, list, 34.0f, w, h, 0.033f);
    }
    EXPECT_TRUE(StepMenuCrop(once, true, true, {872, 450, 1883, 1411}, 34.0f, w, h, 0.033f));
}

// ui_menu_snap, the Inventory from the Options grid and back (mock menufix/runs/t3, band 2720 x
// 1530): the Inventory grows the crop on its second frame; its fade-in then widens its box by a few
// px (x0 147 -> 141, x1 2570 -> 2576 as its soft edges cross the alpha threshold), which moved the
// crop twice more (4 and 5 stage px, 117 and 134 ms later): within half the margin it stays, and
// content that really reaches out still grows it (never cut). B back: the grid's first frame takes
// the crop the menu locked on, at once (ui_menu_snap=0: the 0.6 s shrink to the union).
TEST(UiLayout, MenuCropGrowthSlackAndTheFirstPageBack) {
    const float w = 2720.0f, h = 1530.0f;
    const auto step = [&](MenuCropState& st, const std::array<s32, 4>& box, float slack = 0.5f,
                          float quick = MenuQuickShrinkArea) {
        return StepMenuCrop(st, true, true, box, 34.0f, w, h, 0.033f, 2, 0.4f, 0.6f, 0.8f, quick,
                            nullptr, 2, slack);
    };
    const std::array<s32, 4> grid{405, 594, 1369, 1234};
    MenuCropState st{};
    for (int n = 0; n < 3; ++n) {
        step(st, grid);
    }
    ASSERT_TRUE(st.valid);
    ASSERT_TRUE(st.root_valid);
    const MenuCropState placed = st;
    EXPECT_FALSE(step(st, {147, 62, 2570, 1442}));
    EXPECT_TRUE(step(st, {147, 62, 2570, 1442}));
    EXPECT_NEAR(st.x0, 147.0f - 34.0f, 1e-3f);
    EXPECT_NEAR(st.x1, 2570.0f + 34.0f, 1e-3f);
    const MenuCropState grown = st;
    // The fade-in's creep: nothing moves, with or without two frames in a row.
    for (int n = 0; n < 10; ++n) {
        EXPECT_FALSE(step(st, {141, 62, 2576, 1442}));
    }
    EXPECT_EQ(st.x0, grown.x0);
    EXPECT_EQ(st.x1, grown.x1);
    // Without the slack (0) the same creep moved it.
    MenuCropState tight = grown;
    EXPECT_FALSE(step(tight, {141, 62, 2576, 1442}, 0.0f));
    EXPECT_TRUE(step(tight, {141, 62, 2576, 1442}, 0.0f));
    EXPECT_NEAR(tight.x0, 141.0f - 34.0f, 1e-3f);
    // Content reaching out of the crop itself still grows it (on two frames).
    MenuCropState out = grown;
    EXPECT_FALSE(step(out, {100, 62, 2576, 1442}));
    EXPECT_TRUE(step(out, {100, 62, 2576, 1442}));
    EXPECT_NEAR(out.x0, 100.0f - 34.0f, 1e-3f);
    // B: the Inventory fades out over the grid (their union), then the grid alone, sliding in.
    EXPECT_FALSE(step(st, {147, 62, 2570, 1442}));
    EXPECT_TRUE(step(st, {450, 594, 1351, 1234}));
    EXPECT_EQ(st.x0, placed.x0);
    EXPECT_EQ(st.y0, placed.y0);
    EXPECT_EQ(st.x1, placed.x1);
    EXPECT_EQ(st.y1, placed.y1);
    for (const s32 x0 : {455, 460, 464, 468, 468, 468, 468}) {
        EXPECT_FALSE(step(st, {x0, 594, x0 + 901, 1234}));
    }
    EXPECT_EQ(st.x0, placed.x0);
    // A page that does not fit the first page's crop: the old rules (still, then the union).
    MenuCropState other = grown;
    EXPECT_FALSE(step(other, {300, 594, 1351, 1234}));
    // quick_area 0 (ui_menu_snap=0): no first-page snap.
    MenuCropState old = grown;
    EXPECT_FALSE(step(old, {450, 594, 1351, 1234}, 0.0f, 0.0f));
}

// Level Up (the Doll, mock menufix/runs/d2, band 2720 x 1530): the book fades in, then the game
// fades a dim in behind it, screen-wide from y 186 down; the opaque UI held still only once the
// dim was there. Waiting for it, the crop would take the dim's box (the whole width: 68 x 34.5
// degrees, text 0.85, the book smaller); it locks on the book instead: its opaque UI with the last
// box before the dim (68 x 35.8 degrees, text 0.94; runs/d3). On the box alone (ui_menu_snap=0)
// it locked mid-fade and moved once more (runs/d1: 'locked', then 'moved' twice).
TEST(UiLayout, MenuCropLocksOnTheMenuNotTheDimBehindIt) {
    const float w = 2720.0f, h = 1530.0f;
    struct Frame {
        std::array<s32, 4> box, opaque;
    };
    const std::array<s32, 4> none{};
    const std::array<s32, 4> dim{0, 186, 2720, 1530};
    const std::array<s32, 4> book{155, 200, 2547, 1427};
    const Frame frames[] = {
        {{155, 215, 2437, 1427}, none},
        {{155, 215, 2437, 1427}, none},
        {{155, 211, 2553, 1427}, none},
        {{155, 211, 2553, 1427}, none},
        {{154, 207, 2554, 1427}, {159, 645, 939, 649}},
        {{154, 207, 2554, 1427}, {159, 645, 939, 649}},
        {{154, 205, 2554, 1427}, {159, 211, 2434, 1260}},
        {{154, 205, 2554, 1427}, {159, 211, 2434, 1260}},
        {{154, 193, 2556, 1427}, {159, 207, 2437, 1267}},
        {{154, 193, 2556, 1427}, {159, 207, 2437, 1267}},
        {dim, book},
        {dim, book},
        {dim, book},
        {dim, book},
    };
    MenuCropState now{}, old{};
    int now_at = -1, now_moves = 0, old_at = -1, old_moves = 0;
    const auto count = [](int i, int& at, int& moves) {
        if (at < 0) {
            at = i;
        } else {
            ++moves;
        }
    };
    for (int i = 0; i < 14; ++i) {
        const auto& f = frames[i];
        if (StepMenuCrop(now, MenuCropTakesBox(now, true, f.box, w, false), true, f.box, 34.0f, w,
                         h, 0.016f, 2, 0.4f, 0.6f, 0.8f, MenuQuickShrinkArea, &f.opaque, 2,
                         MenuGrowSlack)) {
            count(i, now_at, now_moves);
        }
        if (StepMenuCrop(old, MenuCropTakesBox(old, true, f.box, w, false), true, f.box, 34.0f, w,
                         h, 0.016f)) {
            count(i, old_at, old_moves);
        }
    }
    EXPECT_EQ(now_at, 12); // the opaque UI's third still frame
    EXPECT_EQ(now_moves, 0);
    EXPECT_NEAR(now.x0, 154.0f - 34.0f, 1e-3f);
    EXPECT_NEAR(now.y0, 193.0f - 34.0f, 1e-3f);
    EXPECT_NEAR(now.x1, 2556.0f + 34.0f, 1e-3f);
    EXPECT_NEAR(now.y1, 1427.0f + 34.0f, 1e-3f);
    // On the box alone: locked mid-fade (two boxes 2 px apart), then moved up 12 px.
    EXPECT_EQ(old_at, 6);
    EXPECT_EQ(old_moves, 1);
    EXPECT_NEAR(old.y0, 193.0f - 34.0f, 1e-3f);
    // After the lock the dim is kept out as before (MenuCropTakesBox), and the crop stays.
    EXPECT_FALSE(MenuCropTakesBox(now, true, dim, w, false));
    // A menu that is all dim (no opaque UI apart from it, or its opaque UI screen-wide too) still
    // locks on its box.
    MenuCropState all{};
    for (int n = 0; n < 3; ++n) {
        StepMenuCrop(all, true, true, dim, 34.0f, w, h, 0.016f, 2, 0.4f, 0.6f, 0.8f,
                     MenuQuickShrinkArea, &dim, 2, MenuGrowSlack);
    }
    ASSERT_TRUE(all.valid);
    EXPECT_EQ(all.x0, 0.0f);
    EXPECT_EQ(all.x1, w);
}

// ui_menu_snap's quick shrink only for a page much smaller than the crop that holds still: a
// blinking element never shrinks it, a moderately smaller page keeps the 0.6 s rule.
TEST(UiLayout, MenuCropQuickShrinkOnlyForAStillSmallPage) {
    const float w = 2560.0f, h = 1440.0f;
    const std::array<s32, 4> inventory{100, 27, 2457, 1389};
    const auto placed_on = [&](const std::array<s32, 4>& box) {
        MenuCropState st{};
        for (int n = 0; n < 3; ++n) {
            StepMenuCrop(st, true, true, box, 32.0f, w, h, 0.016f);
        }
        return st;
    };
    // A small element blinking over a small page: never still, so the 0.6 s union rule.
    MenuCropState blink = placed_on(inventory);
    const std::array<s32, 4> page{373, 560, 1280, 1173};
    const std::array<s32, 4> page_blink{373, 500, 1280, 1173};
    int steps = 0;
    while (!StepMenuCrop(blink, true, true, steps % 2 ? page : page_blink, 32.0f, w, h, 0.016f, 2,
                         0.4f, 0.6f, 0.8f, MenuQuickShrinkArea) &&
           steps < 100) {
        ++steps;
    }
    EXPECT_GE(steps * 0.016f, 0.55f);
    EXPECT_NEAR(blink.y0, 500.0f - 32.0f, 1e-3f); // the union: the blinking element kept
    // A page at 70% of the crop, still: the 0.6 s rule too (not much smaller).
    MenuCropState moderate = placed_on(inventory);
    const std::array<s32, 4> seventy{100, 27, 2100, 1250};
    steps = 0;
    while (!StepMenuCrop(moderate, true, true, seventy, 32.0f, w, h, 0.016f, 2, 0.4f, 0.6f, 0.8f,
                         MenuQuickShrinkArea) &&
           steps < 100) {
        ++steps;
    }
    EXPECT_GE(steps * 0.016f, 0.55f);
    // The grid again, still: at the third equal box.
    MenuCropState quick = placed_on(inventory);
    EXPECT_FALSE(StepMenuCrop(quick, true, true, page, 32.0f, w, h, 0.016f, 2, 0.4f, 0.6f, 0.8f,
                              MenuQuickShrinkArea));
    EXPECT_FALSE(StepMenuCrop(quick, true, true, page, 32.0f, w, h, 0.016f, 2, 0.4f, 0.6f, 0.8f,
                              MenuQuickShrinkArea));
    EXPECT_TRUE(StepMenuCrop(quick, true, true, page, 32.0f, w, h, 0.016f, 2, 0.4f, 0.6f, 0.8f,
                             MenuQuickShrinkArea));
    EXPECT_NEAR(quick.x0, 341.0f, 1e-3f);
    EXPECT_NEAR(quick.x1, 1312.0f, 1e-3f);
    // Off (quick_area 0, ui_menu_snap=0): 0.6 s as before.
    MenuCropState old = placed_on(inventory);
    steps = 0;
    while (!StepMenuCrop(old, true, true, page, 32.0f, w, h, 0.016f) && steps < 100) {
        ++steps;
    }
    EXPECT_GE(steps * 0.016f, 0.55f);
}

TEST(UiLayout, DeathTitle) {
    // The words 40 degrees across at 4 m: 2 * 4 * tan(20) = 2.91 m for 692 stage px.
    const DeathTitleSize q = DeathTitleQuad(4.0f, 40.0f);
    const float m_px = 2.0f * 4.0f * std::tan(20.0f * DegToRadF) / DeathTextWidthPx;
    EXPECT_NEAR(q.width_m, DeathTitleRect.Width() * m_px, 1e-4f);
    EXPECT_NEAR(q.height_m, DeathTitleRect.Height() * m_px, 1e-4f);
    // The cut-out holds the words (stage x 612..1304, y 205..304) with room for their glow.
    EXPECT_LE(DeathTitleRect.x0, 612.0f - 30.0f);
    EXPECT_GE(DeathTitleRect.x1, 1304.0f + 30.0f);
    EXPECT_LE(DeathTitleRect.y0, 205.0f - 20.0f);
    EXPECT_GE(DeathTitleRect.y1, 304.0f + 20.0f);
    EXPECT_NEAR(DeathTextWidthPx * m_px, 2.912f, 0.01f);
    // The cut-out is centred on the words (stage centre (958, 254)) and leaves the dim below.
    EXPECT_NEAR(0.5f * (DeathTitleRect.x0 + DeathTitleRect.x1), 960.0f, 3.0f);
    EXPECT_NEAR(0.5f * (DeathTitleRect.y0 + DeathTitleRect.y1), 254.0f, 3.0f);
    EXPECT_GE(DeathTitleRect.x0, 0.0f);
    EXPECT_LE(DeathTitleRect.x1, StageWidth);
    EXPECT_LT(DeathTitleRect.y1, DeathDimTop);
    EXPECT_LE(DeathTitleRect.Width(), StageWidth);
    // Out-of-range text sizes are clamped (never a degenerate or a wrapped-around quad).
    EXPECT_GT(DeathTitleQuad(4.0f, 0.0f).width_m, 0.0f);
    EXPECT_LT(DeathTitleQuad(4.0f, 500.0f).width_m, 2.0f * 4.0f * std::tan(1.05f) * 2.1f);

    DeathTitleState st{};
    // Off (ui_death_distance_m=0): never.
    EXPECT_FALSE(StepDeathTitle(st, false, true, true, true));
    EXPECT_FALSE(st.on);
    // A screen-wide frame while alive (the fade in after a load): not a title.
    EXPECT_FALSE(StepDeathTitle(st, true, true, false, true));
    EXPECT_FALSE(st.on);
    // Dead but the gameplay HUD still up (the death animation): not yet.
    EXPECT_FALSE(StepDeathTitle(st, true, false, true, true));
    // No head pose to place it: waits.
    EXPECT_FALSE(StepDeathTitle(st, true, true, true, false));
    EXPECT_FALSE(st.on);
    // The death screen: starts once (placed then), stays put.
    EXPECT_TRUE(StepDeathTitle(st, true, true, true, true));
    EXPECT_TRUE(st.on);
    EXPECT_FALSE(StepDeathTitle(st, true, true, true, true));
    EXPECT_TRUE(st.on);
    // The loading page in the same period (HP read back already): keeps the title.
    EXPECT_FALSE(StepDeathTitle(st, true, true, false, true));
    EXPECT_TRUE(st.on);
    // A clear border must stay clear before ending the title.
    EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, false, 0.36f));
    EXPECT_FALSE(st.on);
    EXPECT_FALSE(StepDeathTitle(st, true, true, false, true));
    EXPECT_FALSE(st.on);
}

TEST(UiLayout, BannerTitle) {
    // The cut-out holds the widest banner seen with its glow (PREY SLAUGHTERED x 252..1667, BLOOD
    // ECHOES RETRIEVED x 385..1536, letters y 214..312), centred, clear of the HUD the game keeps
    // showing (the item name down to y 190, the mark at x 115).
    EXPECT_LE(BannerTitleRect.x0, 252.0f - 30.0f);
    EXPECT_GE(BannerTitleRect.x1, 1667.0f + 30.0f);
    EXPECT_GT(BannerTitleRect.x0, 130.0f);
    EXPECT_GT(BannerTitleRect.y0, 190.0f);
    EXPECT_LE(BannerTitleRect.y0, 214.0f - 12.0f);
    EXPECT_GE(BannerTitleRect.y1, 312.0f + 15.0f);
    EXPECT_NEAR(0.5f * (BannerTitleRect.x0 + BannerTitleRect.x1), 960.0f, 3.0f);
    EXPECT_NEAR(0.5f * (BannerTitleRect.y0 + BannerTitleRect.y1), 266.0f, 3.0f);
    EXPECT_LT(BannerTitleRect.y1, DeathDimTop);
    EXPECT_EQ(TitleRect(true).x0, DeathTitleRect.x0);
    EXPECT_EQ(TitleRect(false).x0, BannerTitleRect.x0);
    // PREY SLAUGHTERED 50 degrees across at 4 m: 2 * 4 * tan(25) = 3.73 m for 1415 px; the death
    // title as before.
    const DeathTitleSize b = TitleQuad(false, 0, 4.0f, 40.0f, 50.0f);
    const float m_px = 2.0f * 4.0f * std::tan(25.0f * DegToRadF) / BannerTextWidthPx;
    EXPECT_NEAR(b.width_m, BannerTitleRect.Width() * m_px, 1e-4f);
    EXPECT_NEAR(b.height_m, BannerTitleRect.Height() * m_px, 1e-4f);
    EXPECT_NEAR(BannerTextWidthPx * m_px, 3.731f, 0.01f);
    EXPECT_NEAR(TitleQuad(true, 2, 4.0f, 40.0f, 50.0f).width_m,
                DeathTitleQuad(4.0f, 40.0f).width_m, 1e-6f);
    // The measured banners' cut-outs: tight around their letters (+ glow), centred, inside the
    // widest; the same scale (the same letter size).
    for (const u32 type : {1u, 4u}) {
        const StageRect r = TitleRect(false, type);
        EXPECT_NEAR(0.5f * (r.x0 + r.x1), 960.0f, 1.0f);
        EXPECT_GE(r.x0, BannerTitleRect.x0);
        EXPECT_LE(r.x1, BannerTitleRect.x1);
        EXPECT_EQ(r.y0, BannerTitleRect.y0);
        EXPECT_EQ(r.y1, BannerTitleRect.y1);
        EXPECT_NEAR(TitleQuad(false, type, 4.0f, 40.0f, 50.0f).width_m, r.Width() * m_px, 1e-4f);
    }
    EXPECT_LE(TitleRect(false, 1).x0, 252.0f - 30.0f); // PREY SLAUGHTERED x 252..1667
    EXPECT_GE(TitleRect(false, 1).x1, 1667.0f + 30.0f);
    EXPECT_LE(TitleRect(false, 4).x0, 385.0f - 30.0f); // BLOOD ECHOES RETRIEVED x 385..1536
    EXPECT_GE(TitleRect(false, 4).x1, 1536.0f + 30.0f);
    EXPECT_EQ(TitleRect(false, 5).x0, BannerTitleRect.x0); // not measured: the widest
    DeathTitleState st{};
    const float dt = 1.0f / 60.0f;
    // A banner (the echoes regained) while alive, no screen-wide frame: a title at once.
    EXPECT_TRUE(StepDeathTitle(st, true, false, false, true, true, dt));
    EXPECT_TRUE(st.on);
    EXPECT_FALSE(st.screen);
    // It stays BannerHoldS, placed once.
    float t = dt;
    while (t < BannerHoldS - 0.1f) {
        EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, false, dt));
        t += dt;
    }
    EXPECT_TRUE(st.on);
    // A second banner meanwhile (prey slaughtered, then the echoes): the same place, time again.
    EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, true, dt));
    for (int i = 0; i < 60; ++i) {
        EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, false, dt));
    }
    EXPECT_TRUE(st.on);
    for (int i = 0; i < 400 && st.on; ++i) {
        EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, false, dt));
    }
    EXPECT_FALSE(st.on);
    // No head pose: a banner is not placed (a later one is).
    EXPECT_FALSE(StepDeathTitle(st, true, false, false, false, true, dt));
    EXPECT_FALSE(st.on);
    // YOU DIED: a banner before the death screen's dim reaches the edges (HP may still read
    // above 0), then the screen-wide period keeps it past the hold until that period ends.
    EXPECT_TRUE(StepDeathTitle(st, true, false, false, true, true, dt));
    EXPECT_FALSE(StepDeathTitle(st, true, true, true, true, false, dt));
    EXPECT_TRUE(st.screen);
    for (int i = 0; i < 1200; ++i) {
        EXPECT_FALSE(StepDeathTitle(st, true, true, i < 600, true, false, dt));
    }
    EXPECT_TRUE(st.on);
    for (int i = 0; i < 30; ++i)
        EXPECT_FALSE(StepDeathTitle(st, true, false, false, true, false, dt));
    EXPECT_FALSE(st.on);
    // Off (ui_death_distance_m=0): banners are not titles either.
    EXPECT_FALSE(StepDeathTitle(st, false, false, false, true, true, dt));
    EXPECT_FALSE(st.on);
}

TEST(UiLayout, ScreenWideGameplayUi) {
    // The 2560 x 1440 band's 8 px border.
    constexpr u32 w = 2560, h = 1440;
    const u64 border = BorderPixels(w, h);
    EXPECT_EQ(border, 2560ull * 1440ull - 2544ull * 1424ull);
    EXPECT_EQ(BorderPixels(10, 10), 100ull);
    ScreenWideState st{};
    u64 seq = 0;
    // Gameplay HUD: nothing at the edges; half the border is not enough either.
    EXPECT_FALSE(StepScreenWide(st, ++seq, 0, w, h));
    EXPECT_FALSE(StepScreenWide(st, ++seq, border / 2, w, h));
    EXPECT_FALSE(st.on);
    // The death screen's backdrop covers the whole border: on at once.
    EXPECT_TRUE(StepScreenWide(st, ++seq, border, w, h));
    EXPECT_TRUE(st.on);
    // The same frame again (no new measurement): nothing.
    EXPECT_FALSE(StepScreenWide(st, seq, 0, w, h));
    // The hand-over to the loading page: 5% of the border for 8 frames keeps it on.
    for (int n = 0; n < 8; ++n) {
        EXPECT_FALSE(StepScreenWide(st, ++seq, border / 20, w, h));
    }
    EXPECT_TRUE(st.on);
    EXPECT_FALSE(StepScreenWide(st, ++seq, border, w, h));
    // 20% (above the off level) never turns it off.
    for (int n = 0; n < 30; ++n) {
        StepScreenWide(st, ++seq, border / 5, w, h);
    }
    EXPECT_TRUE(st.on);
    // Back in gameplay: off after ScreenWideOffFrames measured frames without the backdrop.
    u32 frames = 0;
    while (st.on && frames < 100) {
        StepScreenWide(st, ++seq, 0, w, h);
        ++frames;
    }
    EXPECT_EQ(frames, ScreenWideOffFrames);
    // No band: never on.
    ScreenWideState none{};
    EXPECT_FALSE(StepScreenWide(none, 1, border, 0, 0));
}

TEST(UiLayout, CountersAreTheRightCluster) {
    // ui_counters hides the echoes / insight cluster: CountersSegment is status_right with both
    // anchors and every scale.
    for (const auto anchor : {HudAnchor::View, HudAnchor::Band}) {
        for (const float scale : {0.035f, 0.042f, 0.06f}) {
            const auto hud = ComputeHudLayout(HudLayout::Belt, scale, 0.0f, anchor);
            ASSERT_GT(hud.count, CountersSegment);
            EXPECT_STREQ(hud.segment[CountersSegment].name, "status_right");
            EXPECT_LE(hud.segment[CountersSegment].rect.x0, 1549.0f); // the counters' content
            EXPECT_GE(hud.segment[CountersSegment].rect.x1, 1808.0f);
        }
    }
}

TEST(UiLayout, CountersShowOnlyWhileLookingDown) {
    // The user (2026-10-05): 40 degrees down, not 35; hidden again above about 34.
    constexpr float Show = 40.0f;
    bool shown = false;
    // Looking ahead, up, or down less than the threshold (35 degrees too): hidden.
    for (const float pitch : {0.0f, 20.0f, -10.0f, -30.0f, -35.0f, -39.9f}) {
        shown = StepCountersLookDown(shown, pitch, Show);
        EXPECT_FALSE(shown) << pitch;
    }
    // Down past it: shown, and it stays while the head hovers around it (hysteresis).
    EXPECT_TRUE(shown = StepCountersLookDown(shown, -40.0f, Show));
    for (const float pitch : {-60.0f, -90.0f, -39.0f, -36.0f, -34.5f}) {
        shown = StepCountersLookDown(shown, pitch, Show);
        EXPECT_TRUE(shown) << pitch;
    }
    // Back up past the threshold less the hysteresis: hidden again.
    EXPECT_FALSE(shown = StepCountersLookDown(shown, -Show + CountersHysteresisDeg, Show));
    EXPECT_FALSE(StepCountersLookDown(false, -37.0f, Show));
    // The modes: dialogs always show them; Always / Hidden ignore the head.
    EXPECT_TRUE(CountersWanted(CountersMode::LookDown, false, true));
    EXPECT_FALSE(CountersWanted(CountersMode::LookDown, false, false));
    EXPECT_TRUE(CountersWanted(CountersMode::LookDown, true, false));
    EXPECT_TRUE(CountersWanted(CountersMode::Always, false, false));
    EXPECT_FALSE(CountersWanted(CountersMode::Hidden, false, true));
    EXPECT_TRUE(CountersWanted(CountersMode::Hidden, true, false));
    // The settings' defaults: look_down at 40 degrees, in the tray (lower middle), the HP
    // cluster with them (auto: look_down with the wrist gauge).
    const VR::Settings d{};
    EXPECT_EQ(static_cast<CountersMode>(d.ui_counters), CountersMode::LookDown);
    EXPECT_FLOAT_EQ(d.ui_counters_pitch_deg, 40.0f);
    EXPECT_EQ(static_cast<CountersPos>(d.ui_counters_pos), CountersPos::LowerMiddle);
    EXPECT_EQ(StatusLeftOf(d.ui_status_left, true), StatusLeftMode::LookDown);
    EXPECT_EQ(StatusLeftOf(d.ui_status_left, false), StatusLeftMode::Always);
}

TEST(UiLayout, StatusLeftFollowsTheCounters) {
    // ui_status_left: the HP cluster hidden in play and shown with the counters by looking down
    // (look_down), as the flat game (always), or never in play (hidden); a small dialog or note
    // shows it in every mode, in its corner.
    EXPECT_EQ(StatusLeftOf(0, false), StatusLeftMode::LookDown);
    EXPECT_EQ(StatusLeftOf(1, true), StatusLeftMode::Always);
    EXPECT_EQ(StatusLeftOf(2, true), StatusLeftMode::Hidden);
    EXPECT_TRUE(StatusLeftWanted(StatusLeftMode::LookDown, false, true));
    EXPECT_FALSE(StatusLeftWanted(StatusLeftMode::LookDown, false, false));
    EXPECT_TRUE(StatusLeftWanted(StatusLeftMode::LookDown, true, false));
    EXPECT_TRUE(StatusLeftWanted(StatusLeftMode::Always, false, false));
    EXPECT_FALSE(StatusLeftWanted(StatusLeftMode::Hidden, false, true));
    EXPECT_TRUE(StatusLeftWanted(StatusLeftMode::Hidden, true, false));
    // In the tray only when shown by looking down (and while fading out after it).
    bool tray = false;
    tray = CountersRaised(StatusLeftAsCounters(StatusLeftMode::LookDown), true, true, tray);
    EXPECT_TRUE(tray);
    tray = CountersRaised(StatusLeftAsCounters(StatusLeftMode::LookDown), false, false, tray);
    EXPECT_TRUE(tray);
    tray = CountersRaised(StatusLeftAsCounters(StatusLeftMode::LookDown), false, true, tray);
    EXPECT_FALSE(tray); // a dialog: the corner
    EXPECT_FALSE(CountersRaised(StatusLeftAsCounters(StatusLeftMode::Always), true, true, false));
}

// A tray cluster's content as flat view angles (centre +- stage px x deg per px): the HP cluster
// x 106 .. its right end, y 63 .. 219; the counters x 1549 .. 1808, y 68 .. 160.
struct Box {
    float l{}, r{}, top{}, bottom{};
};

Box ContentOf(const Segment& seg, float x0, float x1, float y0, float y1) {
    const float cx = 0.5f * (seg.rect.x0 + seg.rect.x1);
    const float cy = 0.5f * (seg.rect.y0 + seg.rect.y1);
    return {seg.yaw_deg + (x0 - cx) * seg.deg_per_px, seg.yaw_deg + (x1 - cx) * seg.deg_per_px,
            seg.row_pitch_deg + (cy - y0) * seg.deg_per_px,
            seg.row_pitch_deg + (cy - y1) * seg.deg_per_px};
}

Box HpBox(const HudSegments& hud, float right_x) {
    return ContentOf(hud.segment[StatusLeftSegment], HudLeftContentX, right_x, HudContentTopY,
                     HudContentBottomY);
}

Box CountersBox(const HudSegments& hud) {
    return ContentOf(hud.segment[CountersSegment], CountersContentLeftX, HudRightContentX,
                     CountersContentTopY, CountersContentBottomY);
}

TEST(UiLayout, TrayIsCentredLowAndLarger) {
    const VR::Settings d{};
    TrayConfig cfg{};
    cfg.top_deg = d.ui_counters_tray_top_deg;
    cfg.scale = d.ui_counters_scale;
    cfg.gap_deg = d.ui_counters_gap_deg;
    cfg.stacked = d.ui_counters_tray_stack;
    EXPECT_TRUE(cfg.stacked);
    const auto base = ComputeHudLayout(HudLayout::Belt, d.ui_hud_scale_deg, d.ui_hud_pitch_deg,
                                       HudAnchor::View, d.ui_hud_corner_yaw_deg,
                                       d.ui_hud_corner_pitch_deg);
    // The defaults: one narrow column in the middle, the HP cluster over the counters.
    auto hud = base;
    PlaceTray(hud, true, true, HudLeftContentRightDefaultX, cfg);
    const Box hp = HpBox(hud, HudLeftContentRightDefaultX);
    const Box cnt = CountersBox(hud);
    EXPECT_NEAR(hp.l + hp.r, 0.0f, 0.01f); // each centred
    EXPECT_NEAR(cnt.l + cnt.r, 0.0f, 0.01f);
    EXPECT_NEAR(hp.top, cfg.top_deg, 0.01f);
    EXPECT_NEAR(hp.bottom - cnt.top, cfg.gap_deg, 0.01f); // the counters right under it
    // About 30% larger than the corner HUD: body text 1.3 degrees (was 1.0).
    EXPECT_NEAR(hud.segment[CountersSegment].EmDeg() / base.segment[CountersSegment].EmDeg(), 1.3f,
                0.001f);
    EXPECT_NEAR(hud.segment[StatusLeftSegment].EmDeg(), 1.31f, 0.02f);
    // Down there: wholly below the view's centre and below the action prompts (9 to 12.5
    // degrees down), so a prompt never sits on it; narrow (the hands are at the sides: looking
    // down 42 degrees the mock's scripted poses hold them 15 to 30 degrees out, and 11 of 14
    // poses leave the column clear in the eyes' depth, scratchpad health/tray_depth.py); within
    // 30 degrees of the view's centre (the comfortable eye turn, the canvas's corner limit).
    EXPECT_LT(hp.top, -13.0f);
    EXPECT_GT(cnt.bottom, -30.0f);
    EXPECT_LT(hp.r, 10.5f);
    EXPECT_LT(cnt.r, 10.5f);
    // The tray's rows replace the corner's: the lower group is untouched.
    EXPECT_EQ(hud.segment[2].row_pitch_deg, base.segment[2].row_pitch_deg);
    EXPECT_EQ(hud.segment[2].deg_per_px, base.segment[2].deg_per_px);
    // A longer HP bar (about 2000 HP: the cluster to x 800): still centred, the counters stay.
    auto long_hp = base;
    PlaceTray(long_hp, true, true, 800.0f, cfg);
    const Box hp_long = HpBox(long_hp, 800.0f);
    EXPECT_NEAR(hp_long.l + hp_long.r, 0.0f, 0.01f);
    EXPECT_NEAR(CountersBox(long_hp).l, cnt.l, 0.01f);
    // Side by side (ui_counters_tray_stack=0): HP cluster | gap | counters, centred as a group,
    // their tops level.
    TrayConfig side = cfg;
    side.stacked = false;
    auto row = base;
    PlaceTray(row, true, true, HudLeftContentRightDefaultX, side);
    const Box hp_row = HpBox(row, HudLeftContentRightDefaultX);
    const Box cnt_row = CountersBox(row);
    EXPECT_NEAR(hp_row.l + cnt_row.r, 0.0f, 0.01f);
    EXPECT_NEAR(cnt_row.l - hp_row.r, side.gap_deg, 0.01f);
    EXPECT_NEAR(hp_row.top, side.top_deg, 0.01f);
    EXPECT_NEAR(row.segment[CountersSegment].row_pitch_deg,
                row.segment[StatusLeftSegment].row_pitch_deg, 0.001f);
    // Only the counters (ui_status_left=always keeps the HP cluster in its corner): centred, at
    // the top.
    auto only = base;
    PlaceTray(only, false, true, HudLeftContentRightDefaultX, cfg);
    const Box cnt_only = CountersBox(only);
    EXPECT_NEAR(cnt_only.l + cnt_only.r, 0.0f, 0.01f);
    EXPECT_NEAR(cnt_only.top, cfg.top_deg, 0.01f);
    EXPECT_EQ(only.segment[StatusLeftSegment].yaw_deg, base.segment[StatusLeftSegment].yaw_deg);
    // Only the HP cluster (ui_counters=hidden): centred.
    auto left = base;
    PlaceTray(left, true, false, HudLeftContentRightDefaultX, cfg);
    const Box hp_only = HpBox(left, HudLeftContentRightDefaultX);
    EXPECT_NEAR(hp_only.l + hp_only.r, 0.0f, 0.01f);
    // Neither: nothing moves.
    auto none = base;
    PlaceTray(none, false, false, HudLeftContentRightDefaultX, cfg);
    EXPECT_EQ(none.segment[CountersSegment].yaw_deg, base.segment[CountersSegment].yaw_deg);
}

TEST(UiLayout, TrayWidthFollowsTheHpBarButNeverMovesWhileShown) {
    TrayLatch latch{};
    EXPECT_NEAR(latch.right_x, HudLeftContentRightDefaultX, 1e-6f);
    // Hidden: follows the measured content (a level up lengthened the bar; it shrinks back).
    EXPECT_NEAR(StepTrayLatch(latch, false, 500.0f), 500.0f, 1e-6f);
    EXPECT_NEAR(StepTrayLatch(latch, false, 420.0f), 420.0f, 1e-6f);
    // Nothing measured: kept.
    EXPECT_NEAR(StepTrayLatch(latch, false, -1.0f), 420.0f, 1e-6f);
    // Shown: only grows (a wider item name), never shrinks while you read it.
    EXPECT_NEAR(StepTrayLatch(latch, true, 400.0f), 420.0f, 1e-6f);
    EXPECT_NEAR(StepTrayLatch(latch, true, 450.0f), 450.0f, 1e-6f);
    EXPECT_NEAR(StepTrayLatch(latch, true, 2000.0f), 960.0f, 1e-6f); // within its rect
}

TEST(UiLayout, CountersRaisedOnlyWhenShownByLookingDown) {
    // Looking down: raised; fading out after looking up: still raised (no jump while visible);
    // a dialog without looking down: the corner; the other modes: the corner.
    bool raised = false;
    raised = CountersRaised(CountersMode::LookDown, true, true, raised);
    EXPECT_TRUE(raised);
    raised = CountersRaised(CountersMode::LookDown, false, false, raised); // fading out
    EXPECT_TRUE(raised);
    raised = CountersRaised(CountersMode::LookDown, false, true, raised); // a dialog
    EXPECT_FALSE(raised);
    raised = CountersRaised(CountersMode::LookDown, false, false, raised);
    EXPECT_FALSE(raised);
    EXPECT_TRUE(CountersRaised(CountersMode::LookDown, true, true, false)); // a dialog, looking down
    EXPECT_FALSE(CountersRaised(CountersMode::Always, true, true, true));
    EXPECT_FALSE(CountersRaised(CountersMode::Hidden, true, false, true));
    // Raised by the default, the counters' content (stage y 68..160 about the row at y 135) stays
    // above the right hand looking down 45 degrees, hanging relaxed (about 27 degrees below the
    // view's centre) or held low in front (the mock's "still" pose: about 4 degrees below it, with
    // 8 degrees of fade margin), within the view both eyes share, right of the lower group's
    // prompts (within 19.4 degrees of the centre; flat angles here, the cylinder puts the
    // counters' left end at 20.2 measured).
    const VR::Settings d{};
    const auto hud = ComputeHudLayout(HudLayout::Belt, d.ui_hud_scale_deg, d.ui_hud_pitch_deg,
                                      HudAnchor::View, d.ui_hud_corner_yaw_deg,
                                      d.ui_hud_corner_pitch_deg);
    const Segment& seg = hud.segment[CountersSegment];
    const float row = seg.row_pitch_deg + d.ui_counters_raise_deg;
    const float bottom = row - (160.0f - 135.0f) * seg.deg_per_px;
    const float left = seg.yaw_deg + (1549.0f - 0.5f * (seg.rect.x0 + seg.rect.x1)) * seg.deg_per_px;
    const float top = row + (135.0f - 68.0f) * seg.deg_per_px;
    EXPECT_GT(bottom, -3.8f + 8.0f);
    EXPECT_LT(top, 15.0f);
    EXPECT_GE(left, 19.0f);
}

// The HUD fade (ui_hand_fade): the clusters fade as before; the lower group (the action prompts,
// pickups, subtitles, the boss bar), while it holds something to read, keeps the weapon's fade (the
// blade, a gun: 45%) and does not fade for your own hands, arms or body (ui_hud_lower_alpha 1; it
// was 15%); with the boss bar alone it fades as before.
TEST(UiLayout, HudFadeFloorKeepsTheWeaponFadeOnly) {
    const VR::Settings d{};
    EXPECT_FLOAT_EQ(d.ui_hud_lower_alpha, 1.0f);
    const float blade = d.ui_hud_blade_alpha; // 0.45
    EXPECT_FLOAT_EQ(blade, 0.45f);
    const float lower = d.ui_hud_lower_alpha;
    constexpr u32 B = HudInFrontBlade;
    constexpr u32 G = HudInFrontGun;
    constexpr u32 O = HudInFrontOwn;
    // Nothing in front: whole, every piece.
    for (const bool is_lower : {false, true}) {
        for (const bool talking : {false, true}) {
            for (const bool reading : {false, true}) {
                EXPECT_FLOAT_EQ(HudFadeFloor(0, 0, is_lower, talking, reading, blade, lower), 1.0f);
            }
        }
    }
    // The clusters (HP, echoes): the blade 45%, a hand / arm / gun / the body 15%, a card 10%,
    // whatever is in front with it, talking or not, text up or not.
    for (const bool talking : {false, true}) {
        for (const bool reading : {false, true}) {
            EXPECT_FLOAT_EQ(HudFadeFloor(1, B, false, talking, reading, blade, lower), 0.45f);
            for (const u32 parts : {O, G, B | O, G | O, B | G}) {
                EXPECT_FLOAT_EQ(HudFadeFloor(2, parts, false, talking, reading, blade, lower),
                                0.15f);
            }
            EXPECT_FLOAT_EQ(HudFadeFloor(3, 0, false, talking, reading, blade, lower), 0.1f);
        }
    }
    // An action prompt (something to read up): the blade of a weapon held ready across the lower
    // middle keeps it at 45%, as before; so does the gun (the other weapon; it was 15%).
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, false, true, blade, lower), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, G, true, false, true, blade, lower), 0.45f);
    // Your hands, arms or body in front (looking down at an item): whole (it was 15%).
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, true, blade, lower), 1.0f);
    // A weapon and a hand: the weapon's 45% (it was 15%).
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, false, true, blade, lower), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, G | O, true, false, true, blade, lower), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | G | O, true, false, true, blade, lower), 0.45f);
    // Over the open quick-item arc's cards or a held quick item: out of the way as before.
    EXPECT_FLOAT_EQ(HudFadeFloor(3, 0, true, false, true, blade, lower), 0.1f);
    EXPECT_FLOAT_EQ(HudFadeFloor(3, O, true, true, true, blade, lower), 0.1f);
    // While an NPC talks (their subtitles): whole behind the blade, as before, and now behind your
    // hands too; a gun still 45%.
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, true, true, blade, lower), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, true, true, blade, lower), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, G, true, true, true, blade, lower), 0.45f);
    // Nothing to read there (the boss bar alone): as it always was, the clusters' fade, whole
    // behind the blade alone while an NPC talks.
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, false, false, blade, lower), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, false, blade, lower), 0.15f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, G, true, false, false, blade, lower), 0.15f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, false, false, blade, lower), 0.15f);
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, true, false, blade, lower), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, true, false, blade, lower), 0.15f);
    // ui_hud_lower_alpha=0: your hands, arms and body fade it to 15% again (the old fade).
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, true, blade, 0.0f), 0.15f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, false, true, blade, 0.0f), 0.15f);
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, false, true, blade, 0.0f), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, true, true, true, blade, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, true, true, blade, 0.0f), 0.15f);
    // In between: the least your hands, arms and body fade it to; a weapon still 45%.
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, true, blade, 0.7f), 0.7f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, false, true, blade, 0.7f), 0.45f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, true, blade, 0.3f), 0.3f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | O, true, false, true, blade, 0.3f), 0.3f);
    // ui_hud_blade_alpha=1 keeps every piece whole behind the weapons (the clusters behind the
    // blade alone).
    EXPECT_FLOAT_EQ(HudFadeFloor(1, B, false, false, false, 1.0f, lower), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(2, B | G | O, true, false, true, 1.0f, lower), 1.0f);
    // Out of range values stay an opacity.
    EXPECT_FLOAT_EQ(HudFadeFloor(2, O, true, false, true, blade, 1.5f), 1.0f);
    EXPECT_FLOAT_EQ(HudFadeFloor(7, O, true, false, true, blade, 1.0f), 0.1f);
}

// VR::HudTextUp (the presenter's `reading`): something to read is up when the last sample said so
// less than a second ago, and when it is not known (no sample yet, or none for 0.5 s).
TEST(UiLayout, HudTextUpFollowsThePromptAndTheText) {
    constexpr s64 Ms = 1'000'000;
    constexpr s64 T = 600'000 * Ms; // ten minutes in
    // Not known: whole (the fix as it is without the menu detection).
    EXPECT_TRUE(VR::HudTextUp(T, 0, 0));
    EXPECT_TRUE(VR::HudTextUp(T, T - 500 * Ms, 0));
    EXPECT_TRUE(VR::HudTextUp(T, T - 2'000 * Ms, T - 30'000 * Ms));
    // The HUD alone (the boss bar): nothing to read, so it fades as before.
    EXPECT_FALSE(VR::HudTextUp(T, T - 16 * Ms, 0));
    EXPECT_FALSE(VR::HudTextUp(T, T - 499 * Ms, T - 30'000 * Ms));
    // A prompt this frame, and for a second after it went (the game fades it out meanwhile).
    EXPECT_TRUE(VR::HudTextUp(T, T - 16 * Ms, T - 16 * Ms));
    EXPECT_TRUE(VR::HudTextUp(T, T, T));
    EXPECT_TRUE(VR::HudTextUp(T, T - 16 * Ms, T - 1'000 * Ms));
    EXPECT_FALSE(VR::HudTextUp(T, T - 16 * Ms, T - 1'001 * Ms));
}

// Observed at 90 Hz: the 30 Hz opening animation repeats each box three times.
// A frame-count-only lock exposes the menu before its next animation step.
TEST(UiLayout, MenuCropWaitsForAnimationTimeAtHighRefresh) {
    for (const float hz : {90.0f, 120.0f}) {
        MenuCropState crop{};
        const float dt = 1.0f / hz;
        const auto step = [&](const std::array<s32, 4>& box) {
            return StepMenuCrop(crop, true, true, box, 18.2f, 1456, 819, dt,
                                2, 0.4f, 0.6f, 0.8f, MenuQuickShrinkArea, &box,
                                2, MenuGrowSlack, 0.05f);
        };
        const int repeats = static_cast<int>(hz / 30.0f);
        for (int animation = 0; animation < 5; ++animation) {
            const std::array<s32, 4> moving{207 + animation * 3, 318,
                                          720 + animation * 3, 661};
            for (int frame = 0; frame < repeats; ++frame) EXPECT_FALSE(step(moving));
            EXPECT_FALSE(crop.valid);
        }
        const std::array<s32, 4> settled{224, 318, 737, 661};
        bool placed = false;
        for (int frame = 0; frame < 10; ++frame) placed |= step(settled);
        EXPECT_TRUE(placed);
        EXPECT_TRUE(crop.root_valid);
        EXPECT_NEAR(crop.x0, settled[0] - 18.2f, 1e-3f);
        for (int frame = 0; frame < 10; ++frame) EXPECT_FALSE(step(settled));
    }
}

// Captured 1456-wide Options grid: after locking on its settled position,
// the close/back animation starts at x=207 inside the crop's padding. Half-margin
// growth used to recenter it again even though no content was outside the crop.
TEST(UiLayout, MenuAnimationUsesPaddingWithoutRecentering) {
    MenuCropState st{};
    const auto step = [&](const std::array<s32, 4>& b) {
        return StepMenuCrop(st, true, true, b, 18.2f, 1456, 819, 1.0f / 90.0f,
                            2, 0.4f, 0.6f, 0.8f, MenuQuickShrinkArea, &b,
                            2, MenuGrowSlack, 0.05f);
    };
    for (int i = 0; i < 12; ++i) step({217, 318, 733, 661});
    ASSERT_TRUE(st.valid);
    const auto placed = st;
    for (const int x : {207, 207, 207, 208, 209, 212, 214, 217}) {
        EXPECT_FALSE(step({x, 318, x + 516, 661}));
        EXPECT_FLOAT_EQ(st.x0, placed.x0);
        EXPECT_FLOAT_EQ(st.x1, placed.x1);
    }
    // A genuinely larger page is still revealed, with padding.
    EXPECT_FALSE(step({78, 34, 1375, 772}));
    EXPECT_TRUE(step({78, 34, 1375, 772}));
    EXPECT_LT(st.x0, 78);
    EXPECT_GT(st.x1, 1375);
}

TEST(UiLayout, MenuPopsAfterAnimationAndHidesOnNextAction) {
    MenuPopState st{};
    BeginMenuPop(st);
    const float dt = 1.0f / 90;
    for (int i = 0; i < 18; ++i) {
        const std::array<s32, 4> b{207 + 3 * (i / 3), 318, 720 + 3 * (i / 3), 661};
        EXPECT_FALSE(StepMenuPop(st, true, true, b, b, dt));
    }
    const std::array<s32, 4> settled{225, 318, 738, 661};
    bool shown = false;
    for (int i = 0; i < 10; ++i) shown |= StepMenuPop(st, true, true, settled, settled, dt);
    EXPECT_TRUE(shown);
    BeginMenuPop(st); // Back/Close hides the canvas on the input frame.
    EXPECT_FALSE(StepMenuPop(st, false, true, settled, settled, dt));
    // A command ignored by the game cannot leave its menu invisible.
    for (int i = 0; i < 60; ++i) StepMenuPop(st, false, false, {}, {}, dt);
    EXPECT_FALSE(st.pending);
}

} // namespace

TEST(UiLayout, BannerSurvivesFlickeringScreenBorder) {
    DeathTitleState st{};
    constexpr float dt = 1.0f / 90.0f;
    ASSERT_TRUE(StepDeathTitle(st, true, false, false, true, true, dt));
    StepDeathTitle(st, true, true, false, true, false, dt);
    for (int i = 0; i < 20; ++i)
        StepDeathTitle(st, true, false, false, true, false, dt);
    EXPECT_TRUE(st.on); // recorded echoes transition: border disappears for 19 frames
    StepDeathTitle(st, true, true, false, true, false, dt);
    EXPECT_TRUE(st.on);
    for (int i = 0; i < 600; ++i)
        StepDeathTitle(st, true, true, false, true, false, dt);
    for (int i = 0; i < 20; ++i)
        StepDeathTitle(st, true, false, false, true, false, dt);
    EXPECT_TRUE(st.on);
    for (int i = 0; i < 20; ++i)
        StepDeathTitle(st, true, false, false, true, false, dt);
    EXPECT_FALSE(st.on);
}

TEST(UiLayout, AllLargeMessageSlotsShareTitleHandling) {
    constexpr std::array<u32, 8> visible{1, 2, 4, 5, 7, 15, 16, 17};
    for (u32 type = 0; type <= 18; ++type) {
        EXPECT_EQ(BannerTextEffect(type), std::find(visible.begin(), visible.end(), type) != visible.end());
    }
    for (const u32 type : visible) {
        DeathTitleState state{};
        ASSERT_TRUE(StepDeathTitle(state, true, false, false, true, true));
        // A second message must retain its anchor instead of jumping to the current gaze.
        EXPECT_FALSE(StepDeathTitle(state, true, false, false, true, true, 1.0f));
        EXPECT_TRUE(state.on);
        const auto crop = TitleRect(type == 2, type);
        EXPECT_GT(crop.Width(), 0.0f);
        EXPECT_GT(crop.Height(), 0.0f);
        EXPECT_NEAR((crop.x0 + crop.x1) * 0.5f, 960.0f, 1.0f);
        // All visible slots tolerate a gap in the asynchronous border measurement.
        StepDeathTitle(state, true, true, false, true, false, 1.0f);
        StepDeathTitle(state, true, false, false, true, false, 0.2f);
        EXPECT_TRUE(state.on);
        for (int i = 0; i < 8; ++i) {
            StepDeathTitle(state, true, false, false, true, false, 1.0f);
        }
        EXPECT_FALSE(state.on);
    }
}

TEST(UiLayout, RequestedMotionAndShoulderDefaults) {
    const VR::Settings defaults{};
    EXPECT_TRUE(defaults.shoulder_switch);
    EXPECT_FLOAT_EQ(defaults.dodge_dip_m, .13f);
    EXPECT_FLOAT_EQ(defaults.jump_height_scale, 1.f);
}

TEST(UiLayout, DialogueOwnsMarginsAndBottomRowsAtOneDepth) {
    for (auto anchor : {HudAnchor::View, HudAnchor::Band}) {
        auto hud = ComputeHudLayout(HudLayout::Belt, .042f, 0.f, anchor);
        const auto before = hud;
        FitDialogue(hud, true);
        const auto& r = hud.segment[2].rect;
        EXPECT_FLOAT_EQ(r.x0, 0.f);
        EXPECT_FLOAT_EQ(r.x1, StageWidth);
        EXPECT_FLOAT_EQ(r.y0, 700.f);
        EXPECT_FLOAT_EQ(r.y1, StageHeight);
        EXPECT_FLOAT_EQ(hud.segment[2].deg_per_px, before.segment[2].deg_per_px);
        EXPECT_FLOAT_EQ(hud.segment[2].row_pitch_deg, before.segment[2].row_pitch_deg - 1.5f);
        // Both status clusters stay unchanged. Returning to gameplay uses its normal crop.
        EXPECT_FLOAT_EQ(hud.segment[0].rect.x0, before.segment[0].rect.x0);
        EXPECT_FLOAT_EQ(hud.segment[1].rect.x1, before.segment[1].rect.x1);
        auto normal = before;
        FitDialogue(normal, false);
        EXPECT_FLOAT_EQ(normal.segment[2].rect.x0, 300.f);
        EXPECT_FLOAT_EQ(normal.segment[2].rect.y1, 1075.f);
    }
    auto game = ComputeHudLayout(HudLayout::Game, .042f, 0.f);
    FitDialogue(game, true);
    EXPECT_EQ(game.count, 0u);
}

// Entering an area: the name (the lower group's wide element in the stage's lower-right corner)
// comes straight ahead while it is up, and the lower group returns once it has faded out.
TEST(UiLayout, AreaNameComesStraightAhead) {
    AreaNameState st{};
    EXPECT_FALSE(StepAreaName(st, 1440.f, false, .016f)); // an action prompt's right end
    EXPECT_FALSE(StepAreaName(st, 0.f, false, .016f));
    EXPECT_FALSE(StepAreaName(st, 1818.f, true, .016f)); // subtitles
    EXPECT_TRUE(StepAreaName(st, 1818.f, false, .016f)); // the rule's end + the split's margin
    EXPECT_TRUE(StepAreaName(st, 0.f, false, .3f));      // its faint tail
    EXPECT_TRUE(StepAreaName(st, 1818.f, false, .3f));   // seen again: held anew
    EXPECT_TRUE(StepAreaName(st, 0.f, false, .4f));
    EXPECT_FALSE(StepAreaName(st, 0.f, false, .2f));
    for (auto anchor : {HudAnchor::View, HudAnchor::Band}) {
        auto hud = ComputeHudLayout(HudLayout::Belt, .042f, 0.f, anchor);
        const auto before = hud;
        // The GPU splits the lower group up to the stage's right edge, the rule's end included.
        EXPECT_FLOAT_EQ(CutRect(before.segment[2]).x1, StageWidth);
        EXPECT_FLOAT_EQ(CutRect(before.segment[2]).x0, before.segment[2].rect.x0);
        EXPECT_FLOAT_EQ(CutRect(before.segment[0]).x1, before.segment[0].rect.x1);
        // ... and while a name is up to its bottom too (the backing's last rows), only the
        // dynamic one.
        EXPECT_FLOAT_EQ(CutRect(before.segment[2]).y1, before.segment[2].rect.y1);
        EXPECT_FLOAT_EQ(CutRect(before.segment[2], true).y1, StageHeight);
        EXPECT_FLOAT_EQ(CutRect(before.segment[0], true).y1, before.segment[0].rect.y1);
        CentreAreaName(hud);
        const auto& lower = hud.segment[2];
        // The words (x 1420..1706) straight ahead, the rule (x 1125..1794) on the piece; the
        // piece's corners within the shared view, down to the stage's bottom (as the cut).
        EXPECT_LE(lower.rect.x0, 1125.f);
        EXPECT_GE(lower.rect.x1, 1794.f);
        EXPECT_FLOAT_EQ(lower.rect.y1, StageHeight);
        EXPECT_NEAR(Angles(lower, AreaNameCenterX, 950.f).first, 0.f, 1e-3f);
        EXPECT_NEAR(Angles(lower, 1420.f, 925.f).first + Angles(lower, 1706.f, 925.f).first, 0.f,
                    0.1f);
        EXPECT_LE(std::abs(lower.yaw_deg) + 0.5f * lower.WidthDeg(), AreaNameReachDeg + 1e-3f);
        // An action prompt shown with it (x 491..1416) keeps its words (around x 950).
        EXPECT_LE(lower.rect.x0, 900.f);
        // AreaNameDropDeg lower, the same size; the clusters stay.
        EXPECT_NEAR(Angles(lower, 1500.f, 925.f).second,
                    Angles(before.segment[2], 1500.f, 925.f).second - AreaNameDropDeg, 1e-3f);
        EXPECT_FLOAT_EQ(lower.deg_per_px, before.segment[2].deg_per_px);
        EXPECT_FLOAT_EQ(hud.segment[0].yaw_deg, before.segment[0].yaw_deg);
        EXPECT_FLOAT_EQ(hud.segment[1].yaw_deg, before.segment[1].yaw_deg);
    }
}

// The rest quad on a cylinder (core/vr/ui_rest_curve.h): the warp's strips cover the image without
// gaps, and every flat-image point lands on the cylinder in the direction it had on the flat quad.
TEST(UiRestCurve, StripsCoverTheImageInOrder) {
    std::array<RestCurve::Strip, 512> strips{};
    for (const float half_w : {0.6f, 1.0f, 1.28f, 1.6f}) {
        const uint32_t n = RestCurve::Strips(1456, 819, half_w, 4, strips);
        ASSERT_EQ(n, 364u) << half_w;
        EXPECT_EQ(strips[0].src_x0, 0);
        EXPECT_EQ(strips[0].dst_x0, 0);
        EXPECT_EQ(strips[n - 1].src_x1, 1456);
        EXPECT_EQ(strips[n - 1].dst_x1, 1456);
        for (uint32_t i = 1; i < n; ++i) {
            EXPECT_EQ(strips[i].src_x0, strips[i - 1].src_x1);
            EXPECT_EQ(strips[i].dst_x0, strips[i - 1].dst_x1);
            EXPECT_GT(strips[i].src_x1, strips[i].src_x0);
        }
        // Each strip edge within half a source pixel of the exact warp.
        for (uint32_t i = 0; i < n; ++i) {
            EXPECT_LE(std::abs(strips[i].src_x0 -
                               RestCurve::SourceColumn(float(strips[i].dst_x0), 1456.0f, half_w)),
                      0.5f);
        }
    }
    EXPECT_EQ(RestCurve::Strips(0, 819, 1.0f, 4, strips), 0u);
    EXPECT_EQ(RestCurve::Strips(1456, 819, 0.0f, 4, strips), 0u);
}

TEST(UiRestCurve, ColumnsShrinkTowardTheEdgesByTheCosine) {
    std::array<RestCurve::Strip, 512> strips{};
    const float half_w = 1.28f;
    const uint32_t n = RestCurve::Strips(1456, 819, half_w, 4, strips);
    const auto& centre = strips[n / 2];
    const auto& edge = strips[0];
    EXPECT_NEAR(centre.dst_y1 - centre.dst_y0, 819, 1);
    EXPECT_NEAR(float(edge.dst_y1 - edge.dst_y0),
                819.0f * std::cos(std::atan(half_w) * (1.0f - 4.0f / 1456.0f)), 1.0f);
    // Symmetric about the centre row.
    EXPECT_NEAR(edge.dst_y0 + edge.dst_y1, 819, 1);
}

TEST(UiRestCurve, TagsLandInTheirEnemysDirection) {
    // A point of the flat image at (x, y) (tangent units) is seen along (x, y, -1). On the cylinder
    // (radius 1, height 2 Y) it is at angle phi and height h: seen along (sin phi, h, -cos phi).
    const float half_w = 1.28f;
    const float half_h = 0.72f;
    const uint32_t w = 1456;
    const uint32_t h = 819;
    std::array<RestCurve::Strip, 512> strips{};
    const uint32_t n = RestCurve::Strips(w, h, half_w, 4, strips);
    const float max_angle = std::atan(half_w);
    for (const float fx : {-0.95f, -0.6f, -0.2f, 0.0f, 0.35f, 0.8f}) {
        for (const float fy : {-0.9f, -0.3f, 0.0f, 0.5f}) {
            const float sx = (fx + 1.0f) * 0.5f * w; // source pixel
            const float sy = (fy + 1.0f) * 0.5f * h;
            const RestCurve::Strip* s = nullptr;
            for (uint32_t i = 0; i < n; ++i) {
                if (sx >= strips[i].src_x0 && sx < strips[i].src_x1) {
                    s = &strips[i];
                }
            }
            ASSERT_NE(s, nullptr);
            const float u = s->dst_x0 + (sx - s->src_x0) * float(s->dst_x1 - s->dst_x0) /
                                            float(s->src_x1 - s->src_x0);
            const float v = s->dst_y0 + sy * float(s->dst_y1 - s->dst_y0) / float(h);
            const float phi = (2.0f * u / w - 1.0f) * max_angle;
            const float height = (2.0f * v / h - 1.0f) * half_h;
            // Directions compared as flat-image coordinates (tangent units); half a pixel is
            // half_w / 728 = 0.0018.
            const float x = std::tan(phi);
            const float y = height / std::cos(phi);
            EXPECT_NEAR(x, fx * half_w, 0.006f) << fx << " " << fy;
            EXPECT_NEAR(y, fy * half_h, 0.006f) << fx << " " << fy;
        }
    }
    // The cylinder's height is the flat quad's at its centre: radius * angle / aspect = 2 Y.
    EXPECT_NEAR(RestCurve::CentralAngle(half_w) / RestCurve::AspectRatio(half_w, half_h),
                2.0f * half_h, 1e-5f);
}
