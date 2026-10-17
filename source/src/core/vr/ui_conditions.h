// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "common/types.h"
#include "core/vr/ui_layout.h"

// The game's status build-up meter (poison, rapid poison, bleed, frenzy, beasthood) on the UI
// layer as its own small HUD piece under the view's centre (vr.ini ui_conditions=view, the
// default). Pure: no emulator state, unit tested in tests/test_ui_conditions.cpp; the resolve
// (vr_ui_resolve.comp FLAG_CONDITIONS) mirrors the strip's geometry.
//
// The game draws the meter (game asset ConditionGaugeList at stage (978, 132), the top centre) as up
// to five rows, compacted from the top, each an icon (stage x 840..884) and a bar from x 885 whose
// length depends on the character's resistance (L % of the full bar: 885 + 1.9 L + 9). Each frame
// it fills the rows into a static block (icon -1 = row off; L at +0x18). In VR the icon and the
// bar's start fall inside the HP cluster's piece (hidden unless you look down: hud_vitals=wrist)
// and the rest of the bars sits 41 degrees up on the rest quad: you do not see it.
//
// Here: while the game shows at least one row, the rows' stage rect (GaugeRect) is the UI layer's
// cut 3. The resolve moves its pixels into a strip of their own below the atlas's two halves
// (StripTop; zeroed in both halves: nothing of it stays on the rest quad or in the HP cluster's
// piece, whose measured width then stays the HP cluster's), and the presenter shows the strip as
// a piece fixed to the view: its icon column's left edge `left_yaw_deg` from the view's centre,
// row 0's bar `pitch_deg` below it, more rows below (the game adds rows at the bottom, so rows
// never jump), at the HUD's size (ui_hud_scale_deg x ui_conditions_scale). The rect is held for
// HoldNs at its largest (StepHold): the game's block and the drawn frame are read on different
// threads, and a row's last frame must never fall outside the cut.
namespace VR::Ui::Conditions {

constexpr u32 MaxRows = 5;
constexpr float LeftX = 836.0f;       // stage x: the icon column's left edge (icons 840..884)
constexpr float TopY = 106.0f;        // stage y: row 0's top
constexpr float RowPx = 52.55f;       // stage px between rows (Item_0_0..Item_4_0)
constexpr float BottomPadPx = 4.0f;   // below the last row
constexpr float BarBaseX = 894.0f;    // stage x: a bar's end at L 0 (885 + 9)
constexpr float BarPxPerPct = 1.9f;   // stage px of bar per L %
constexpr float EndPadPx = 4.0f;      // right of the longest bar
constexpr float Row0CentreY = 134.0f; // stage y: row 0's bar centre (measured 128..140)
constexpr s32 MaxLen = 100;

// The rows the game shows: `n` rows (0 = none), the longest bar's L %.
struct Rows {
    u32 n{0};
    s32 max_len{0};
};

// From the game's row block: `icon[i]` (-1 = off), `len[i]` (L %). The rows are compacted from
// the top; a hole (never seen) still counts up to the last row shown.
inline Rows CountRows(const std::array<s32, MaxRows>& icon, const std::array<s32, MaxRows>& len) {
    Rows r{};
    for (u32 i = 0; i < MaxRows; ++i) {
        if (icon[i] >= 0) {
            r.n = i + 1;
            r.max_len = std::max(r.max_len, std::clamp(len[i], 0, MaxLen));
        }
    }
    if (r.n == 0) {
        r.max_len = 0;
    }
    return r;
}

// The rows' stage rect (end exclusive); empty (zero size) for no rows.
inline StageRect GaugeRect(const Rows& rows) {
    if (rows.n == 0) {
        return {LeftX, TopY, LeftX, TopY};
    }
    const u32 n = std::min(rows.n, MaxRows);
    const float len = static_cast<float>(std::clamp(rows.max_len, 0, MaxLen));
    return {LeftX, TopY, BarBaseX + BarPxPerPct * len + EndPadPx,
            TopY + BottomPadPx + RowPx * static_cast<float>(n)};
}

// The largest rect (five full-length rows): the strip holds it.
inline StageRect MaxRect() {
    return GaugeRect({MaxRows, MaxLen});
}

// A stage rect in band px (x0, y0, x1, y1, end exclusive), as the resolve's cuts are made
// (vk_presenter_vr.cpp band_rect: floor of the start, ceil of the end).
struct BandRegion {
    s32 x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] s32 Width() const {
        return x1 - x0;
    }
    [[nodiscard]] s32 Height() const {
        return y1 - y0;
    }
};
inline BandRegion ToBand(const StageRect& r, u32 band_width) {
    const float k = static_cast<float>(band_width) / StageWidth;
    return {static_cast<s32>(std::floor(r.x0 * k)), static_cast<s32>(std::floor(r.y0 * k)),
            static_cast<s32>(std::ceil(r.x1 * k)), static_cast<s32>(std::ceil(r.y1 * k))};
}

// The strip below the atlas's two halves: its first row (at least 8 rows below the bottom half,
// on a multiple of 8, so the halves' mips never bleed into it), its size (band px) and the atlas's
// height with it. The resolve computes the same top: (2 * band_h + 15) & ~7.
constexpr u32 StripTop(u32 band_height) {
    return (2 * band_height + 15) & ~7u;
}
inline u32 StripWidth(u32 band_width) {
    return static_cast<u32>(std::max(ToBand(MaxRect(), band_width).Width(), 0));
}
inline u32 StripHeight(u32 band_width) {
    return static_cast<u32>(std::max(ToBand(MaxRect(), band_width).Height(), 0));
}
// The UI atlas's height for a band: its two halves, and the strip when the piece is on. With the
// strip it is a multiple of 8 (the strip's rows padded): the UI swapchain's mips (up to 3 levels,
// each a 2:1 linear blit of the whole image) then halve it exactly, so the halves' mips stay
// where they were (an odd height such as 3451 made each level a hair less than half: the halves'
// mip rows drifted by up to a row toward the atlas's bottom, and the bottom half's last mip row
// took in the unwritten gap below it). The resolve clears the gap rows and the strip's padding
// (vr_ui_resolve.comp StripClear).
inline u32 AtlasHeight(u32 band_width, u32 band_height, bool strip) {
    return strip ? (StripTop(band_height) + StripHeight(band_width) + 7u) & ~7u : 2 * band_height;
}

// The cut's band region for `rows`, inside the strip's size (never larger than it).
inline BandRegion CutRegion(const Rows& rows, u32 band_width) {
    BandRegion r = ToBand(GaugeRect(rows), band_width);
    const BandRegion m = ToBand(MaxRect(), band_width);
    r.x1 = std::min(r.x1, r.x0 + m.Width());
    r.y1 = std::min(r.y1, r.y0 + m.Height());
    return r;
}

// The rect held at its largest for HoldNs (each of n and L on its own): a smaller value takes
// over once the larger one was not seen for HoldNs.
constexpr s64 HoldNs = 300'000'000;
struct Hold {
    Rows held{};
    s64 n_ns{0};
    s64 len_ns{0};
};
inline Rows StepHold(Hold& h, const Rows& now, s64 now_ns) {
    if (now.n >= h.held.n || now_ns - h.n_ns >= HoldNs) {
        h.held.n = now.n;
        h.n_ns = now_ns;
    }
    if (now.max_len >= h.held.max_len || now_ns - h.len_ns >= HoldNs) {
        h.held.max_len = now.max_len;
        h.len_ns = now_ns;
    }
    if (h.held.n == 0) {
        h.held.max_len = 0;
    }
    return h.held;
}

// Where the piece goes (vr.ini ui_conditions_pitch_deg, the HUD's deg per px x
// ui_conditions_scale).
struct Place {
    float pitch_deg{-6.5f};    // row 0's bar centre, from the view's centre (+ = up)
    float left_yaw_deg{-4.5f}; // the icon column's left edge, from the view's centre (+ = right)
    float deg_per_px{0.042f};
};
constexpr float DefaultPitchDeg = -6.5f;
constexpr float DefaultLeftYawDeg = -4.5f;

// The piece for the rect `rect` (GaugeRect): a HUD segment (its centre's yaw right of and row
// pitch from the view's centre). The icon column and row 0 stay put whatever the rect's size.
inline Segment PieceSegment(const StageRect& rect, const Place& p) {
    const float s = std::clamp(p.deg_per_px, 0.005f, 0.2f);
    Segment seg{};
    seg.rect = rect;
    seg.deg_per_px = s;
    seg.yaw_deg = p.left_yaw_deg + (0.5f * (rect.x0 + rect.x1) - LeftX) * s;
    seg.row_pitch_deg = p.pitch_deg - (0.5f * (rect.y0 + rect.y1) - Row0CentreY) * s;
    seg.name = "conditions";
    seg.with_dialogs = true;
    seg.dynamic = false;
    return seg;
}

// The piece's hand fade (UiPieceOccluded's level and parts in the presenter): the action prompts'
// rule (HudFadeFloor's lower group while it holds something to read), since you read it in a fight
// with your hands often in front of it: the blade or a gun in front fades it to blade_alpha
// (ui_hud_blade_alpha, 45%), your own hands, arms or body only to lower_alpha (ui_hud_lower_alpha;
// 1 = not at all), the open quick-item cards or a held quick item (level 3) to 10% as every piece.
constexpr float FadeFloor(u32 level, u32 parts, float blade_alpha, float lower_alpha) {
    return HudFadeFloor(level, parts, true, false, true, blade_alpha, lower_alpha);
}

// vr.ini ui_conditions.
enum class Mode : s32 {
    View = 0, // the piece under the view's centre (the default)
    Game = 1, // where the game draws it (before 2026-10-09: effectively out of view)
};

// Whether the UI atlas carries the strip (the piece is on: ui_conditions=view with the belt,
// ui_hud_layout=belt). The resolve and the UI swapchains use the same answer.
constexpr bool StripWanted(s32 ui_conditions, s32 ui_hud_layout) {
    return ui_conditions == static_cast<s32>(Mode::View) &&
           ui_hud_layout == static_cast<s32>(HudLayout::Belt);
}

} // namespace VR::Ui::Conditions
