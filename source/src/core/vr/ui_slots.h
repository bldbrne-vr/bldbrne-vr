// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "common/types.h"
#include "core/vr/ui_layout.h"

// The HUD widgets' own slots (vr.ini ui_hud_slots). The game draws its UI on the 1920 x 1080 stage,
// scaled to the 16:9 band of the render; a VR eye is taller than 16:9, and the game's Scaleform
// draws there too, above and below the band (measured 2026-10-26: HP moved 420 stage px up showed
// in the rows above the band, the echoes moved down in the rows below). Nothing else of the game
// is drawn there: the world's tags and cursors are projected into the band, the menus and fades
// cover the stage. So the HUD movie's fixed widgets are moved there as it loads
// (core/vr/gfx_hud_slots.h), each into rows no other widget uses, and every piece the presenter
// shows is cut from its own widget's slot: a banner's title never holds the boss bar or the HP
// cluster, the area name never shares the prompt's piece.
//   top margin:    the HUD clusters (PlayerHUD and the autosave icon: HP, stamina, items, echoes,
//                  insight) with the build-up meter (ConditionGaugeList) between them;
//   bottom margin: the banners' words (TextEffect_*, without their screen-wide dims), then the
//                  area name (MapName).
// The lower group (prompts, item pickups, subtitles) stays in the band, where the game lays its
// members out one above the other; the boss bar stays where ui_boss_bar raises it. A widget whose
// slot does not fit the margins of this render stays where the game draws it (today's cut-outs).
// Pure: no emulator state, unit tested in tests/test_ui_slots.cpp.
namespace VR::Ui::Slots {

// The widgets moved, in stage px (their visible content where the game puts them). The clusters
// move as one (PlayerHUD) but are two cut-outs, the meter's slot between them.
constexpr StageRect StatusHome{60.0f, 20.0f, 1840.0f, 266.0f}; // both clusters' rows
constexpr StageRect StatusLeftHome{60.0f, 20.0f, 960.0f, 266.0f};    // HP, stamina, items
constexpr StageRect StatusRightHome{1520.0f, 20.0f, 1840.0f, 266.0f}; // echoes, insight
constexpr StageRect GaugeHome{701.0f, 110.0f, 1087.0f, 368.0f}; // five rows of the meter
constexpr StageRect BannerHome{200.0f, 170.0f, 1720.0f, 340.0f}; // every banner's words (TitleRect)
// The area name without its dark strip (fe.gfx MapName: the smoke from x 1102, the rule from 1125,
// the words right-aligned at 1706; y 831..1039). With the strip left in (VR::IsAreaDimRemoved
// false: it spans the stage's width) the name stays home, keyed in the band as before.
constexpr StageRect AreaHome{1100.0f, 831.0f, StageWidth, 1039.0f};

constexpr float Pad = 4.0f;       // stage px between slots and from the margin's edges
constexpr float GaugeSlotX = 970.0f; // the meter between the clusters (status_left ends at 960)

// The widgets each banner's sprite holds its words at (and their glow); 5 and 7 are its dims.
constexpr std::array<u16, 3> BannerWordDepths{1, 3, 11};
// The root's banner widgets (fe.gfx), every one with its words at BannerWordDepths.
constexpr std::array<const char*, 7> BannerWidgets{
    "TextEffect_Dead",        "TextEffect_KillDemon",  "TextEffect_YouWin", "TextEffect_SoulGet",
    "TextEffect_TargetClear", "TextEffect_BlackClear", "TextEffect_Draw"};

struct Slot {
    bool on{false};
    float dx{0.0f}, dy{0.0f}; // the move (stage px)
    StageRect slot{};         // where its content is drawn now (stage px; y < 0 or > 1080: margin)
    [[nodiscard]] StageRect Moved(const StageRect& home) const {
        return on ? StageRect{home.x0 + dx, home.y0 + dy, home.x1 + dx, home.y1 + dy} : home;
    }
};

struct Layout {
    float margin{0.0f}; // stage px above and below the band in this render
    Slot status;        // PlayerHUD + AutoSaveIcon
    Slot gauge;         // ConditionGaugeList
    Slot banner;        // the banners' words
    Slot area;          // MapName
    [[nodiscard]] bool Any() const {
        return status.on || gauge.on || banner.on || area.on;
    }
};

// The stage px above (and below) the band in a `width` x `height` render (0: 16:9 or wider).
constexpr float MarginOf(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return 0.0f;
    }
    const float stage_h = StageWidth * static_cast<float>(height) / static_cast<float>(width);
    return std::max(0.0f, 0.5f * (stage_h - StageHeight));
}

namespace Detail {
// A slot for `home` with its top at `top` (stage y) and its left at `left` (or home's), if it ends
// within `limit` (stage y, exclusive).
constexpr Slot Place(const StageRect& home, float top, float limit, float left = -1.0f) {
    const float dx = left < 0.0f ? 0.0f : left - home.x0;
    const float dy = top - home.y0;
    Slot s{true, dx, dy, {home.x0 + dx, top, home.x1 + dx, top + home.Height()}};
    if (s.slot.y1 > limit) {
        s = {};
    }
    return s;
}
} // namespace Detail

// The slots for a `width` x `height` render. `area_strip`: the area name keeps its dark strip.
constexpr Layout Compute(u32 width, u32 height, bool area_strip = false) {
    Layout l{};
    l.margin = MarginOf(width, height);
    if (l.margin <= Pad) {
        return l;
    }
    const float top = -l.margin;
    const float bottom = StageHeight;
    // Top margin: the clusters and the meter side by side, both from its top.
    l.status = Detail::Place(StatusHome, top + Pad, -Pad);
    if (l.status.on) {
        l.gauge = Detail::Place(GaugeHome, top + Pad, -Pad, GaugeSlotX);
    }
    // Bottom margin: the banners' words, then the area name under them.
    l.banner = Detail::Place(BannerHome, bottom + Pad, bottom + l.margin - Pad);
    if (l.banner.on && !area_strip) {
        l.area = Detail::Place(AreaHome, l.banner.slot.y1 + Pad, bottom + l.margin - Pad);
    }
    return l;
}

// The area name's own piece (its slot): at the lower group's scale, the words' middle (`centre_x`)
// straight ahead, their top `gap_deg` under the action prompt's last row (PromptBottomY on the
// lower group's segment `lower`), so a prompt shown with it never shares its rows. (Before the
// slots the lower piece itself moved to bring the name ahead, the prompt with it.)
constexpr float PromptBottomY = 955.0f; // ActionButtonTop's last row (stage px)
constexpr float AreaWordsTopY = 908.0f; // the name's words, y 908..956
inline Segment AreaSegment(const Segment& lower, float centre_x = AreaNameCenterX,
                           float gap_deg = 0.5f) {
    Segment a = lower;
    a.rect = AreaHome;
    a.name = "area";
    a.dynamic = false;
    a.with_dialogs = false;
    const float dpp = lower.deg_per_px;
    a.yaw_deg = (0.5f * (AreaHome.x0 + AreaHome.x1) - centre_x) * dpp;
    const float lower_cy = 0.5f * (lower.rect.y0 + lower.rect.y1);
    const float words_top = lower.row_pitch_deg - (PromptBottomY - lower_cy) * dpp - gap_deg;
    const float area_cy = 0.5f * (AreaHome.y0 + AreaHome.y1);
    a.row_pitch_deg = words_top + (AreaWordsTopY - area_cy) * dpp;
    return a;
}

// The two row blocks of the render the slots are in (stage y, end exclusive), stacked in the
// atlas's slot sheet in this order: the top margin's (the clusters and the meter), then the
// bottom margin's (the banners' words and the area name). An empty block has y1 == y0.
struct Blocks {
    float top_y0{}, top_y1{};
    float bottom_y0{}, bottom_y1{};
    [[nodiscard]] float Rows() const {
        return (top_y1 - top_y0) + (bottom_y1 - bottom_y0);
    }
};

constexpr Blocks BlocksOf(const Layout& l) {
    Blocks b{};
    if (l.status.on) {
        b.top_y0 = l.status.slot.y0 - Pad;
        b.top_y1 = std::max(l.status.slot.y1, l.gauge.on ? l.gauge.slot.y1 : l.status.slot.y1) + Pad;
    }
    if (l.banner.on) {
        b.bottom_y0 = l.banner.slot.y0 - Pad;
        b.bottom_y1 = (l.area.on ? l.area.slot.y1 : l.banner.slot.y1) + Pad;
    }
    return b;
}

// A stage rect in a slot (render coordinates, y outside the band) as the sheet has it: x as on
// the stage, y from the sheet's first row (stage px). Rects in neither block are left as they are.
constexpr StageRect InSheet(const Blocks& b, const StageRect& r) {
    if (r.y0 >= b.top_y0 && r.y1 <= b.top_y1 && b.top_y1 > b.top_y0) {
        return {r.x0, r.y0 - b.top_y0, r.x1, r.y1 - b.top_y0};
    }
    const float base = b.top_y1 - b.top_y0;
    if (r.y0 >= b.bottom_y0 && r.y1 <= b.bottom_y1 && b.bottom_y1 > b.bottom_y0) {
        return {r.x0, base + r.y0 - b.bottom_y0, r.x1, base + r.y1 - b.bottom_y0};
    }
    return r;
}

// The slot sheet in band px (k: band px per stage px), for a render `render_h` rows tall whose
// band starts at render row `band_y`: where each block's rows are in the render (the resolve reads
// them there) and in the sheet (the presenter cuts its pieces there). Both use these numbers.
struct Sheet {
    s32 top_src{}, top_rows{};       // the top block: its first render row, its rows
    s32 bottom_src{}, bottom_rows{}; // the bottom block's
    s32 top_y0{}, bottom_y0{};       // the blocks' first rows in band px (from the band's top)
    [[nodiscard]] s32 Rows() const {
        return top_rows + bottom_rows;
    }
};

inline Sheet SheetOf(const Blocks& b, float k, s32 band_y, s32 render_h) {
    Sheet s{};
    const auto rows = [&](float y0, float y1, s32& src, s32& n, s32& first) {
        if (y1 <= y0) {
            return;
        }
        s32 a = static_cast<s32>(std::floor(y0 * k));
        s32 e = static_cast<s32>(std::ceil(y1 * k));
        a = std::max(a, -band_y); // never above the render's first row ...
        e = std::min(e, render_h - band_y); // ... nor below its last
        if (e > a) {
            first = a;
            src = band_y + a;
            n = e - a;
        }
    };
    rows(b.top_y0, b.top_y1, s.top_src, s.top_rows, s.top_y0);
    rows(b.bottom_y0, b.bottom_y1, s.bottom_src, s.bottom_rows, s.bottom_y0);
    return s;
}

// Stage rect `r`, moved into a slot, in sheet px: x0, y0, x1, y1 (end exclusive, clamped to its
// block). All zero: in neither block.
inline std::array<s32, 4> SheetRect(const Sheet& s, const Blocks& b, const StageRect& r, float k) {
    const s32 x0 = static_cast<s32>(std::floor(r.x0 * k));
    const s32 x1 = static_cast<s32>(std::ceil(r.x1 * k));
    const s32 y0 = static_cast<s32>(std::floor(r.y0 * k));
    const s32 y1 = static_cast<s32>(std::ceil(r.y1 * k));
    if (s.top_rows > 0 && r.y0 >= b.top_y0 && r.y1 <= b.top_y1) {
        return {x0, std::clamp(y0 - s.top_y0, 0, s.top_rows), x1,
                std::clamp(y1 - s.top_y0, 0, s.top_rows)};
    }
    if (s.bottom_rows > 0 && r.y0 >= b.bottom_y0 && r.y1 <= b.bottom_y1) {
        return {x0, s.top_rows + std::clamp(y0 - s.bottom_y0, 0, s.bottom_rows), x1,
                s.top_rows + std::clamp(y1 - s.bottom_y0, 0, s.bottom_rows)};
    }
    return {};
}

// The layout the HUD movie was moved to as it loaded (bloodborne_vrtile.cpp): the resolve and the
// presenter cut the same slots. Set once, before the game draws its HUD; none: every widget home.
namespace Detail {
inline Layout g_active{};
inline std::atomic<bool> g_active_set{false};
} // namespace Detail
inline void SetActive(const Layout& l) {
    Detail::g_active = l;
    Detail::g_active_set.store(l.Any(), std::memory_order_release);
}
inline const Layout* Active() {
    return Detail::g_active_set.load(std::memory_order_acquire) ? &Detail::g_active : nullptr;
}

// The game's shot and the submitted screen quad must use the same framing. A manually
// selected screen shows the extra rows only when the HUD actually occupies slots there.
constexpr float ScreenBandAspect = StageWidth / StageHeight;
inline bool CropScreenToBand(float aspect, bool forced_screen) {
    return aspect < ScreenBandAspect - 0.02f && !(forced_screen && Active() != nullptr);
}

} // namespace VR::Ui::Slots
