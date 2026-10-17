// SPDX-License-Identifier: GPL-2.0-or-later

// The status build-up meter as its own HUD piece (src/core/vr/ui_conditions.h, vr.ini
// ui_conditions; vr_ui_resolve.comp FLAG_CONDITIONS mirrors the strip): the rows from the game's
// block, the cut's rect, the strip below the atlas's halves, the hold, the piece's place.

#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "core/vr/ui_conditions.h"

namespace {

using namespace VR::Ui::Conditions;
using VR::Ui::StageRect;
using VR::Ui::StageWidth;
using Icons = std::array<s32, MaxRows>;

constexpr s64 Ms = 1'000'000;

TEST(UiConditions, RowsFromTheGamesBlock) {
    // The kit character, measured in the mock: frenzy (icon 5, L 58) over poison (icon 1, L 48).
    {
        const Icons icon{5, 1, -1, -1, -1};
        const Icons len{58, 48, 0, 0, 0};
        const Rows r = CountRows(icon, len);
        EXPECT_EQ(r.n, 2u);
        EXPECT_EQ(r.max_len, 58);
    }
    {
        const Icons icon{-1, -1, -1, -1, -1};
        const Icons len{58, 48, 79, 0, 0}; // stale lengths of rows that are off
        const Rows r = CountRows(icon, len);
        EXPECT_EQ(r.n, 0u);
        EXPECT_EQ(r.max_len, 0);
    }
    {
        // Beasthood's long bar (L 79) as the only row; a length out of range is clamped.
        const Icons icon{3, -1, -1, -1, -1};
        const Icons len{79, 0, 0, 0, 0};
        EXPECT_EQ(CountRows(icon, len).max_len, 79);
        const Icons wild{250, 0, 0, 0, 0};
        EXPECT_EQ(CountRows(icon, wild).max_len, MaxLen);
    }
    {
        // A hole (never seen: the rows are compacted) still counts up to the last row shown.
        const Icons icon{5, -1, 1, -1, -1};
        const Icons len{58, 0, 48, 0, 0};
        EXPECT_EQ(CountRows(icon, len).n, 3u);
    }
}

TEST(UiConditions, GaugeRectCoversTheRowsExactly) {
    // One frenzy row (L 58): icon from x 840, bar end 885 + 1.9 * 58 + 9 = 1004.2 (measured 1000).
    const StageRect one = GaugeRect({1, 58});
    EXPECT_FLOAT_EQ(one.x0, 836.0f);
    EXPECT_FLOAT_EQ(one.y0, 106.0f);
    EXPECT_NEAR(one.x1, 1004.2f + EndPadPx, 1e-3f);
    EXPECT_NEAR(one.y1, 162.55f, 1e-3f);
    // Row 0's bar (measured stage y 128..140) is inside; so is the icon (x 840..884).
    EXPECT_LT(one.y0, 128.0f);
    EXPECT_GT(one.y1, 140.0f);
    EXPECT_LT(one.x0, 840.0f);
    // Two rows: one more row pitch down.
    const StageRect two = GaugeRect({2, 58});
    EXPECT_NEAR(two.y1 - one.y1, RowPx, 1e-3f);
    // No rows: empty.
    const StageRect none = GaugeRect({0, 0});
    EXPECT_FLOAT_EQ(none.Width(), 0.0f);
    EXPECT_FLOAT_EQ(none.Height(), 0.0f);
    // The largest rect stays inside the stage and clear of the lower group (y >= 700).
    const StageRect m = MaxRect();
    EXPECT_LE(m.x1, StageWidth);
    EXPECT_LT(m.y1, 700.0f);
    EXPECT_NEAR(m.x1, 1088.0f, 1e-3f);
}

TEST(UiConditions, StripBelowTheHalvesAndTheAtlasHeight) {
    // The mock's band (the user's render 2720 wide: band 2720 x 1530).
    constexpr u32 bw = 2720, bh = 1530;
    const u32 top = StripTop(bh);
    EXPECT_GE(top, 2 * bh + 8);
    EXPECT_EQ(top % 8, 0u);
    EXPECT_LT(top, 2 * bh + 16);
    EXPECT_EQ(top, (2 * bh + 15) & ~7u); // what the resolve computes
    const u32 sw = StripWidth(bw), sh = StripHeight(bw);
    // 252 x 266.75 stage px at 1.4167 band px per stage px.
    EXPECT_GE(sw, 357u);
    EXPECT_LE(sw, 359u);
    EXPECT_GE(sh, 378u);
    EXPECT_LE(sh, 380u);
    // The strip's rows padded to a multiple of 8: the mips halve the atlas exactly (2720 x 3456).
    EXPECT_EQ(AtlasHeight(bw, bh, true), (top + sh + 7) & ~7u);
    EXPECT_EQ(AtlasHeight(bw, bh, true), 3456u);
    EXPECT_GE(AtlasHeight(bw, bh, true), top + sh);
    EXPECT_EQ(AtlasHeight(bw, bh, false), 2 * bh); // ui_conditions=game: as before
    for (u32 w : {1920u, 2048u, 2720u, 3200u, 4096u}) {
        const u32 h = w * 9 / 16;
        const u32 a = AtlasHeight(w, h, true);
        EXPECT_EQ(a % 8, 0u);
        EXPECT_GE(a, StripTop(h) + StripHeight(w));
        EXPECT_LT(a, StripTop(h) + StripHeight(w) + 8);
    }
    // Every cut fits the strip.
    for (u32 n = 1; n <= MaxRows; ++n) {
        for (s32 len : {0, 20, 58, 79, 100}) {
            const BandRegion r = CutRegion({n, len}, bw);
            EXPECT_LE(static_cast<u32>(r.Width()), sw);
            EXPECT_LE(static_cast<u32>(r.Height()), sh);
            EXPECT_GT(r.Width(), 0);
            EXPECT_GT(r.Height(), 0);
        }
    }
    // Other band widths (a 1920 render, a large one).
    for (u32 w : {1920u, 2048u, 3200u, 4096u}) {
        const BandRegion big = CutRegion({MaxRows, MaxLen}, w);
        EXPECT_LE(static_cast<u32>(big.Width()), StripWidth(w));
        EXPECT_LE(static_cast<u32>(big.Height()), StripHeight(w));
    }
}

TEST(UiConditions, CutRegionIsTheBandRectOfTheRows) {
    // At 1920 the band is the stage.
    const BandRegion r = CutRegion({1, 58}, 1920);
    EXPECT_EQ(r.x0, 836);
    EXPECT_EQ(r.y0, 106);
    EXPECT_EQ(r.x1, 1009); // ceil(1008.2)
    EXPECT_EQ(r.y1, 163);  // ceil(162.55)
}

TEST(UiConditions, HoldKeepsTheLargestForHoldNs) {
    Hold h{};
    s64 t = 1000 * Ms;
    EXPECT_EQ(StepHold(h, {2, 58}, t).n, 2u);
    // A torn read (the filler cleared the rows) or the row's last frames: still 2.
    t += 16 * Ms;
    EXPECT_EQ(StepHold(h, {0, 0}, t).n, 2u);
    t += 16 * Ms;
    EXPECT_EQ(StepHold(h, {1, 48}, t).max_len, 58);
    // A new row shows at once.
    t += 16 * Ms;
    EXPECT_EQ(StepHold(h, {3, 79}, t).n, 3u);
    EXPECT_EQ(StepHold(h, {3, 79}, t).max_len, 79);
    // Gone for HoldNs: gone.
    t += HoldNs;
    const Rows gone = StepHold(h, {0, 0}, t);
    EXPECT_EQ(gone.n, 0u);
    EXPECT_EQ(gone.max_len, 0);
}

TEST(UiConditions, PieceSitsUnderTheViewCentre) {
    Place p{};
    p.deg_per_px = 0.042f;
    p.pitch_deg = DefaultPitchDeg;
    p.left_yaw_deg = DefaultLeftYawDeg;
    const StageRect one = GaugeRect({1, 58});
    const VR::Ui::Segment seg = PieceSegment(one, p);
    // Its left edge (the icon column) at -4.5 deg, row 0's bar centre at -6.5 deg.
    EXPECT_NEAR(seg.yaw_deg - 0.5f * seg.WidthDeg(), -4.5f, 1e-4f);
    const float top = seg.row_pitch_deg + 0.5f * seg.HeightDeg();
    EXPECT_NEAR(top - (Row0CentreY - TopY) * p.deg_per_px, -6.5f, 1e-4f);
    // A typical bar ends a few degrees right of the centre; a full one at about +6.
    EXPECT_GT(seg.yaw_deg + 0.5f * seg.WidthDeg(), 1.0f);
    EXPECT_LT(seg.yaw_deg + 0.5f * seg.WidthDeg(), 3.5f);
    const VR::Ui::Segment full = PieceSegment(MaxRect(), p);
    EXPECT_NEAR(full.yaw_deg + 0.5f * full.WidthDeg(), -4.5f + 252.0f * 0.042f, 1e-3f);
    // More rows or a longer bar never move the icon column or row 0.
    const VR::Ui::Segment more = PieceSegment(GaugeRect({3, 79}), p);
    EXPECT_NEAR(more.yaw_deg - 0.5f * more.WidthDeg(), -4.5f, 1e-4f);
    EXPECT_NEAR(more.row_pitch_deg + 0.5f * more.HeightDeg(), top, 1e-4f);
    // Row 0 sits above the action prompts (-9 .. -12.5 deg): its bar's bottom (stage y 140).
    EXPECT_GT(top - (140.0f - TopY) * p.deg_per_px, -9.0f);
    EXPECT_STREQ(seg.name, "conditions");
    EXPECT_TRUE(seg.with_dialogs);
    EXPECT_FALSE(seg.dynamic);
}

TEST(UiConditions, FadesForTheWeaponNotYourHands) {
    using VR::Ui::Conditions::FadeFloor;
    constexpr u32 Blade = VR::Ui::HudInFrontBlade, Gun = VR::Ui::HudInFrontGun,
                  Own = VR::Ui::HudInFrontOwn;
    // Nothing in front: whole.
    EXPECT_FLOAT_EQ(FadeFloor(0, 0, 0.45f, 1.0f), 1.0f);
    // The blade (level 1) or the gun (level 2): ui_hud_blade_alpha.
    EXPECT_FLOAT_EQ(FadeFloor(1, Blade, 0.45f, 1.0f), 0.45f);
    EXPECT_FLOAT_EQ(FadeFloor(2, Gun, 0.45f, 1.0f), 0.45f);
    // Your own hand, arm or body (level 2): whole by default (ui_hud_lower_alpha 1) ...
    EXPECT_FLOAT_EQ(FadeFloor(2, Own, 0.45f, 1.0f), 1.0f);
    // ... ui_hud_lower_alpha 0 = the other pieces' 15%, never below it.
    EXPECT_FLOAT_EQ(FadeFloor(2, Own, 0.45f, 0.0f), VR::Ui::HudFadeLevel2Alpha);
    EXPECT_FLOAT_EQ(FadeFloor(2, Own, 0.45f, 0.6f), 0.6f);
    // A hand and the blade together: the blade's.
    EXPECT_FLOAT_EQ(FadeFloor(2, Own | Blade, 0.45f, 1.0f), 0.45f);
    EXPECT_FLOAT_EQ(FadeFloor(2, Own | Gun, 0.30f, 1.0f), 0.30f);
    // The quick-item cards / a held quick item (level 3): 10%, as every piece.
    EXPECT_FLOAT_EQ(FadeFloor(3, Own, 0.45f, 1.0f), VR::Ui::HudFadeLevel3Alpha);
    // The same as the action prompts' rule while they hold something to read.
    for (u32 level = 0; level <= 3; ++level) {
        for (u32 parts = 0; parts <= (Blade | Gun | Own); ++parts) {
            EXPECT_FLOAT_EQ(FadeFloor(level, parts, 0.45f, 1.0f),
                            VR::Ui::HudFadeFloor(level, parts, true, false, true, 0.45f, 1.0f));
        }
    }
}

} // namespace
