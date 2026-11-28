// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <span>

#include "core/vr/game_font.h"
#include "core/vr/vr_page_art.h"
#include "core/vr/vr_page_draw.h"
#include "core/vr/vr_page_render.h"

namespace VR::Page {

using namespace Draw;

namespace {

// ---- Layout (stage px), from the game's Environment page (optionsetting.gfx) -------------------
// (the title, help line and key guide: core/vr/vr_page_draw.h)

constexpr float HeaderX = 533.2f; // StaticText_111010 / 111011
constexpr float LabelRight = 594.3f - 29.0f + 322.0f; // Text_0 #38, right aligned
constexpr float LabelDy = 19.1f + Ascent24;
// The two groups (2026-10-09, the tester: "the 'show enemy health bars' is still very close to its
// header above"). Each group header sits HeaderAboveLabel over its first row's label, baseline to
// baseline: the movie's 30.15 (StaticText_111010) + 8, so a header's descenders ("Display",
// "Body") clear a long label's ascenders by about half a line (12.5..13.25 px on the glyphs, were
// 4.5..5.25), on every tab. Both headers sit the same 31.42 px under their divider's top (the
// frame's rules at 254.7 and 404.55; the second header where it was, the first 5.6 px higher than
// the movie's 291.72), their ascenders ~8 px under the rule's line. The room comes from the rows,
// all moved together (art, labels, glows, hit boxes): the first group's
// two 2.4 px lower, the second group's three 8 px lower (its last row's art still ~11 px above the
// bottom rule, as far as the first group's last row is above the second divider); each group
// keeps the movie's row pitch.
constexpr std::array<float, Slots> RowShift{2.4f, 2.4f, 8.0f, 8.0f, 8.0f};
constexpr float HeaderAboveLabel = 30.15f + 8.0f;
constexpr float ValueCentreX = 594.3f + 410.9f + 48.5f + 3.5f + 2.0f + 125.0f; // ComboBox text
constexpr float ValueDy = -11.0f + 24.5f + 2.0f + 2.0f + Ascent24;
constexpr float NumberCentreX = 594.3f + 410.9f + 62.5f + 274.8f + 16.45f; // the slider's number
constexpr float NumberDy = -11.0f + 40.5f - 6.4f + Ascent24 * 23.0f / 24.0f;
constexpr float ButtonTextDx = -23.0f + 160.0f; // Text_0 #99 centre from the button's origin
constexpr float ButtonTextDy = -31.6f + Ascent24;

// The rendering state's line (the Upscaling tab) between the last row and the buttons.
constexpr float GraphicsStatusBaseline = 646.0f + StripBandPx;
// The rooms (stage px) of the texts drawn smaller when they are wider (Render): a choice's value
// (to 16 px), an Action row's label (to 17 px), the help line (to 17 px), the rendering state
// (20 px, to 18 px).
constexpr float ValueRoom = 290.0f;
constexpr float ActionRoom = 800.0f;
constexpr float HelpRoom = 1418.1f - 501.6f - 16.0f; // the ornament rules, 8 px inside
constexpr float StatusRoom = 880.0f;

float ButtonOriginY(const View& view) {
    return PageArt::ButtonY + StripBandPx + (view.graphics && !view.confirm ? 12.0f : 0.0f);
}

// The sub-tab strip in its band under the title's rule: the pages' names centred on the panel, a
// gap between them; the shown one in the title's gold with a hairline under it.
constexpr float StripBaseline = StripRuleY + 6.0f + 0.5f * StripBandPx + 0.5f * Ascent24 - 2.0f;
constexpr float StripGap = 72.0f;
struct StripLayout {
    std::vector<float> x, width;
};
StripLayout LayoutStrip(const View& view, const GameText::GameFont* font) {
    StripLayout l;
    float total = 0.0f;
    for (const auto& name : view.subtabs) {
        l.width.push_back(TextWidth(font, name));
        total += l.width.back();
    }
    total += StripGap * static_cast<float>(std::max<size_t>(view.subtabs.size(), 1) - 1);
    float x = PanelCentreX - 0.5f * total;
    for (const float w : l.width) {
        l.x.push_back(x);
        x += w + StripGap;
    }
    return l;
}

float ButtonHalfWidth(const ButtonView& button) {
    return button.label == "SAVE AND RESTART" ? 230.0f : 150.0f;
}

float ButtonCentreX(int which) {
    const float right = PageArt::ButtonX + ButtonTextDx;
    return which == 0 ? 2.0f * MirrorX - right : right;
}

} // Anonymous namespace

View MakeView(Model& model, const Glyphs& glyphs) {
    View v;
    const State& s = model.GetState();
    const Tab& tab = model.CurrentTab();
    for (const Tab& t : Tabs()) {
        v.tabs.emplace_back(t.name);
    }
    v.tab = s.tab;
    v.glyphs = glyphs;
    v.hover = s.hover;
    v.confirm = s.confirm;
    v.page = s.page;
    v.pages = tab.Pages();
    if (v.pages > 1) {
        for (int p = 0; p < v.pages; ++p) {
            v.subtabs.emplace_back(tab.PageName(p));
        }
    }
    v.strip_selected = s.focus == FocusStrip && !s.confirm;
    v.graphics = tab.left.action == ButtonAction::RestartGraphics ||
                 tab.left.action == ButtonAction::AcceptGraphics ||
                 tab.right.action == ButtonAction::RestartGraphics ||
                 tab.right.action == ButtonAction::AcceptGraphics;
    const auto& headers = tab.HeadersOf(s.page);
    for (int h = 0; h < 2; ++h) {
        v.headers[static_cast<size_t>(h)] =
            headers[static_cast<size_t>(h)] != nullptr ? headers[static_cast<size_t>(h)] : "";
    }
    Store& store = model.GetStore();
    for (int slot = 0; slot < Slots; ++slot) {
        const Row* row = model.RowAt(slot);
        RowView& rv = v.rows[static_cast<size_t>(slot)];
        if (row == nullptr) {
            continue;
        }
        rv.present = true;
        rv.label = RowLabel(*row, store); // {A}+{B}: the buttons of the layout
        rv.widget = row->widget;
        rv.value = ValueText(*row, store);
        rv.enabled = Enabled(*row, store);
        rv.selected = rv.enabled && !s.confirm && s.focus == slot;
        if (row->widget == Widget::Slider) {
            rv.number = CurrentNumber(*row, store);
            rv.fraction = row->max > row->min ? static_cast<float>(rv.number - row->min) /
                                                    static_cast<float>(row->max - row->min)
                                              : 0.0f;
        }
    }
    if (s.confirm) {
        v.buttons[0] = {true, "YES", s.confirm_yes == 1};
        v.buttons[1] = {true, "NO", s.confirm_yes == 0};
        v.help = s.confirm_restart ? "Save and restart the game? Progress since the last autosave may be lost."
                                   : v.pages > 1 ? "Return this tab (all its pages) to its defaults?"
                                                 : "Return this page to its defaults?";
        v.keys = {{KeyGlyph::DpadLR, "Select"}, {KeyGlyph::Confirm, "OK"}, {KeyGlyph::Back, "Return"}};
        return v;
    }
    v.buttons[0] = {tab.left.label != nullptr, tab.left.label != nullptr ? tab.left.label : "",
                    s.focus == FocusLeftButton};
    v.buttons[1] = {tab.right.label != nullptr, tab.right.label != nullptr ? tab.right.label : "",
                    s.focus == FocusRightButton};
    if (v.strip_selected) {
        v.help = "This tab's pages: left or right switches between them.";
        v.keys = {{KeyGlyph::DpadLRValues, "Page"}, {KeyGlyph::DpadUD, "Select"},
                  {KeyGlyph::Back, "Return"}};
    } else if (s.focus == FocusLeftButton || s.focus == FocusRightButton) {
        const Button& b = s.focus == FocusLeftButton ? tab.left : tab.right;
        v.help = b.help != nullptr ? b.help : "";
        v.keys = {{KeyGlyph::DpadUD, "Select"}, {KeyGlyph::Confirm, "OK"}, {KeyGlyph::Back, "Return"}};
    } else if (const Row* row = model.RowAt(s.focus)) {
        v.help = RowHelp(*row, model.GetStore());
        // Change: the stick's left / right and the triggers (LT / RT).
        v.keys = {{KeyGlyph::DpadLRValues, "Change"}, {KeyGlyph::DpadUD, "Select"},
                  {KeyGlyph::Back, "Return"}};
    }
    if (tab.left.action == ButtonAction::AcceptGraphics ||
        tab.right.action == ButtonAction::AcceptGraphics) {
        v.graphics_status = model.GraphicsPending()
            ? "Unsaved changes | ACCEPT to save. Return discards changes."
            : store.Get("graphics_status");
        if (const Row* scale = model.RowAt(s.focus);
            scale != nullptr && std::string_view(scale->id) == "graphics_scale")
            v.help = "-50% to +50% per dimension. Save and restart to change scene/output resolution.";
    }
    if (!s.notice.empty()) {
        v.help = s.notice; // the owner's one-off line
    }
    return v;
}

float RowTop(int slot) {
    const auto s = static_cast<size_t>(std::clamp(slot, 0, Slots - 1));
    return PageArt::RowY[s] + RowShift[s] + StripBandPx;
}

float HeaderBaseline(int group) {
    return LabelBaseline(std::clamp(group, 0, 1) * 2) - HeaderAboveLabel;
}

float LabelBaseline(int slot) {
    return RowTop(slot) + LabelDy;
}

std::array<float, 2> SliderBarSpan(int slot) {
    (void)slot;
    const float x0 = PageArt::RowX + PageArt::SliderFill[0].x0;
    return {x0, x0 + PageArt::SliderFillWidth};
}

std::vector<HitBox> HitBoxes(const View& view, const GameText::GameFont* font) {
    std::vector<HitBox> boxes;
    const TabLayout t = LayoutTabs(view.tabs, view.glyphs, font, view.tab);
    for (size_t i = 0; i < view.tabs.size(); ++i) {
        if (t.width[i] <= 0.0f)
            continue;
        boxes.push_back({{Target::Tab, static_cast<int>(i)}, t.x[i] - 8.0f, TitleBaseline - 26.0f,
                         t.x[i] + t.width[i] + 8.0f, TitleBaseline + 10.0f});
    }
    boxes.push_back({{Target::TabPrev, -1}, t.prev_cx - 0.5f * t.prev_w - 4.0f, t.badge_cy - 18.0f,
                     t.prev_cx + 0.5f * t.prev_w + 4.0f, t.badge_cy + 18.0f});
    boxes.push_back({{Target::TabNext, -1}, t.next_cx - 0.5f * t.next_w - 4.0f, t.badge_cy - 18.0f,
                     t.next_cx + 0.5f * t.next_w + 4.0f, t.badge_cy + 18.0f});
    if (view.pages > 1 && !view.confirm) {
        const StripLayout l = LayoutStrip(view, font);
        for (size_t p = 0; p < l.x.size(); ++p) {
            boxes.push_back({{Target::PageTab, static_cast<int>(p)}, l.x[p] - 0.5f * StripGap,
                             StripBaseline - 34.0f, l.x[p] + l.width[p] + 0.5f * StripGap,
                             StripBaseline + 14.0f});
        }
    }
    for (int slot = 0; slot < Slots; ++slot) {
        const RowView& r = view.rows[static_cast<size_t>(slot)];
        if (!r.present || !r.enabled) {
            continue;
        }
        const float y = RowTop(slot);
        const float x = PageArt::RowX;
        if (r.widget == Widget::Action) {
            boxes.push_back({{Target::Row, slot},
                             PageArt::FrameX0 + 40.f,
                             y + 4.f,
                             PageArt::FrameX1 - 40.f,
                             y + 52.f});
            continue;
        }
        if (r.widget == Widget::Slider) {
            const auto bar = SliderBarSpan(slot);
            boxes.push_back({{Target::RowLeft, slot}, x + PageArt::SliderArrowLeft[0].x0 - 10.0f,
                             y + 12.0f, x + PageArt::SliderArrowLeft[0].x1 + 6.0f, y + 54.0f});
            boxes.push_back({{Target::RowRight, slot}, x + PageArt::SliderArrowRight[0].x0 - 6.0f,
                             y + 12.0f, x + PageArt::SliderArrowRight[0].x1 + 10.0f, y + 54.0f});
            boxes.push_back({{Target::SliderBar, slot}, bar[0] - 6.0f, y + 14.0f, bar[1] + 6.0f,
                             y + 52.0f});
        } else {
            boxes.push_back({{Target::RowLeft, slot}, x + PageArt::ComboArrowLeft[0].x0 - 12.0f,
                             y + 10.0f, x + PageArt::ComboArrowLeft[0].x1 + 8.0f, y + 52.0f});
            boxes.push_back({{Target::RowRight, slot}, x + PageArt::ComboArrowRight[0].x0 - 8.0f,
                             y + 10.0f, x + PageArt::ComboArrowRight[0].x1 + 12.0f, y + 52.0f});
        }
        boxes.push_back({{Target::Row, slot}, PageArt::FrameX0 + 40.0f, y + 4.0f,
                         PageArt::FrameX1 - 40.0f, y + 52.0f});
    }
    for (int b = 0; b < 2; ++b) {
        if (!view.buttons[static_cast<size_t>(b)].present) {
            continue;
        }
        const float cx = ButtonCentreX(b);
        const float half_width = ButtonHalfWidth(view.buttons[static_cast<size_t>(b)]);
        boxes.push_back({{b == 0 ? Target::ButtonLeft : Target::ButtonRight, -1}, cx - half_width,
                         ButtonOriginY(view) - 36.0f, cx + half_width, ButtonOriginY(view) + 14.0f});
    }
    return boxes;
}

Hover HitTest(const std::vector<HitBox>& boxes, float x, float y) {
    for (const HitBox& b : boxes) {
        if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1) {
            return b.hover;
        }
    }
    if (x >= PageArt::FrameX0 && x < PageArt::FrameX1 && y >= PageArt::FrameY0 &&
        y < PageArt::FrameY1 + StripBandPx) {
        return {Target::Page, -1};
    }
    return {};
}

// A centred row's selection on a single-column page (the lamp page): a soft gold band behind the
// label and gilt hairlines above and below it, all fading out toward their ends (the settings
// page's row glow sits in its label column, away from a centred word, and read as faint: the
// user, 2026-10-14). `alpha` scales it (the page itself, then a lighter copy on the glow).
void SelectionBand(Canvas& c, float cx_stage, float cy_stage, float half_w_stage, float alpha,
                   float half_height = 17.0f) {
    const float cx = Canvas::ToX(cx_stage), cy = Canvas::ToY(cy_stage);
    const float hw = half_w_stage * PageScale, hh = half_height * PageScale;
    const int X0 = static_cast<int>(std::floor(cx - hw)), X1 = static_cast<int>(std::ceil(cx + hw));
    const int Y0 = static_cast<int>(std::floor(cy - hh - 2)),
              Y1 = static_cast<int>(std::ceil(cy + hh + 2));
    for (int y = Y0; y < Y1; ++y) {
        const float v = (y + 0.5f - cy) / hh;
        const float band = std::exp(-1.6f * v * v) * 0.34f;
        const float line = std::max(Coverage(std::abs(std::abs(y + 0.5f - cy) - hh) - 0.7f), 0.0f);
        for (int x = X0; x < X1; ++x) {
            const float u = (x + 0.5f - cx) / hw;
            const float ends = std::max(0.0f, 1.0f - u * u);
            const float fade = ends * std::sqrt(ends);
            c.Over(x, y, TitleGold.r, TitleGold.g, TitleGold.b, band * fade * alpha);
            if (line > 0.0f) {
                c.Over(x, y, Gilt.r, Gilt.g, Gilt.b, 0.8f * line * fade * alpha);
            }
        }
    }
}

bool Render(const Assets& assets, const View& v, Image& page_out, Image& glow_out) {
    if (!assets.Ready()) {
        return false;
    }
    const GameText::GameFont* font = assets.font;
    Canvas page;
    Canvas glow;

    // 1. The frame (the same for every view: drawn once per atlas and copied).
    {
        static std::mutex cache_mutex;
        // One frame per kind: the settings page's and a single-column page's (View::single_column).
        static std::array<const u8*, 2> cache_atlases{};
        static std::array<std::vector<float>, 2> caches;
        const size_t kind = v.single_column ? 1 : 0;
        const u8*& cache_atlas = cache_atlases[kind];
        std::vector<float>& cache = caches[kind];
        std::scoped_lock lk{cache_mutex};
        if (cache_atlas != assets.common3.rgba || cache.size() != page.px.size()) {
            // The game's screen dim behind its settings pages (#19, 60% black over everything),
            // here only inside the panel's own soft footprint: the page is a layer of its own.
            // In VR the dim is full where the panel is solid, so the panel's middle is opaque (the
            // game's 70% panel over its 60% dim let 12% of the world through the text's backing;
            // in the headset that world, at another depth and redrawn every frame, read as the
            // page flickering and differing per eye: the play test of 2026-10-09); its soft edges
            // keep the game's dim (PanelDim). VR_PAGE_SEE_THROUGH=1: the game's dim everywhere.
            // The frame with the sub-tab strip's band (TallFrame: Frame's order, then the
            // strip's lower rule).
            const auto& frame = TallFrameCached();
            for (int i = 0; i < PageArt::FrameBackdropCount; ++i) {
                DrawPiece(page, assets, frame[static_cast<size_t>(i)], 0.0f, 0.0f, false,
                          PageArt::ScreenDimAlpha, 1e9f, true, PanelOpaque());
            }
            const int n = static_cast<int>(frame.size());
            for (int i = 0; i < n; ++i) {
                // A single-column page without the column divider and the texture's split halves.
                const bool texture =
                    i >= PageArt::FrameTextureFirst && i < PageArt::FrameTextureEnd;
                const bool divider =
                    i >= PageArt::FrameDividerFirst && i < PageArt::FrameDividerEnd;
                if (!v.single_column || (!texture && !divider)) {
                    DrawPiece(page, assets, frame[static_cast<size_t>(i)], 0.0f, 0.0f, false,
                              1.0f);
                }
            }
            cache = page.px;
            cache_atlas = assets.common3.rgba;
        } else {
            page.px = cache;
        }
    }

    // 2. The tab strip in the title line: the active tab in the title's gold with a gilt
    //    hairline under it, the grips' badges (LG / RG) either side.
    DrawTabStrip(page, font, v.tabs, v.tab, v.glyphs);

    // 3. Group headers.
    for (int h = 0; h < 2; ++h) {
        DrawText(page, font, v.headers[static_cast<size_t>(h)], HeaderX, HeaderBaseline(h),
                 Align::Left, HeaderTeal);
    }
    // 3b. The sub-tab strip (a tab with pages): the shown page in gold with a hairline, the
    //     others grey; selected, a soft gold band behind it (as the lamp page's rows).
    if (v.pages > 1) {
        const StripLayout l = LayoutStrip(v, font);
        if (v.strip_selected) {
            const float half = 0.5f * (l.x.back() + l.width.back() - l.x.front()) + 90.0f;
            SelectionBand(page, PanelCentreX, StripBaseline - 8.0f, half, 1.0f);
            SelectionBand(glow, PanelCentreX, StripBaseline - 8.0f, half, 0.5f);
        }
        for (size_t p = 0; p < l.x.size(); ++p) {
            const bool shown = static_cast<int>(p) == v.page;
            const bool hovered = v.hover.target == Target::PageTab && v.hover.index == static_cast<int>(p);
            DrawText(page, font, v.subtabs[p], l.x[p], StripBaseline, Align::Left,
                     shown ? TitleGold : hovered ? LabelSelected : TabDim);
            if (shown) { // the hairline of the tab strip's shown tab (DrawTabStrip)
                const int y = static_cast<int>(Canvas::ToY(StripBaseline + 8.0f));
                const int x0 = static_cast<int>(Canvas::ToX(l.x[p] + 4.5f)),
                          x1 = static_cast<int>(Canvas::ToX(l.x[p] + l.width[p] - 4.5f));
                for (int x = x0; x < x1; ++x) {
                    page.Over(x, y, 150 / 255.0f, 122 / 255.0f, 70 / 255.0f, 200 / 255.0f);
                    page.Over(x, y + 1, 150 / 255.0f, 122 / 255.0f, 70 / 255.0f, 110 / 255.0f);
                }
            }
        }
    }

    // 4. Rows.
    for (int slot = 0; slot < Slots; ++slot) {
        const RowView& r = v.rows[static_cast<size_t>(slot)];
        if (!r.present) {
            continue;
        }
        const float ox = PageArt::RowX, oy = RowTop(slot);
        // A single-column page's rows are centred: the settings page's underline (from the label
        // column's left) only drew stray lines under them.
        if (!v.single_column) {
            DrawPieces(page, assets, PageArt::RowLine, ox, oy);
        }
        const Rgb ink = !r.enabled ? TabDim : r.selected ? LabelSelected : LabelGrey;
        if (r.widget == Widget::Action) {
            constexpr float room = ActionRoom;
            const float width = TextWidth(font, r.label);
            const float size = width > room ? std::max(17.f, 24.f * room / width) : 24.f;
            if (v.single_column) {
                // The selected row: a gold band and hairlines behind a gilt word; the others a
                // little dimmer, so which one is active reads at a glance.
                const bool on = r.selected && r.enabled;
                const float text_w = std::min(width, room) * size / 24.f;
                const bool has_detail = !r.value.empty();
                const float cy = has_detail ? oy + 28.0f : oy + LabelDy - 8.0f;
                if (on) {
                    const float half_width = 0.5f * text_w + 140.0f;
                    const float half_height = has_detail ? 23.0f : 17.0f;
                    SelectionBand(page, PanelCentreX, cy, half_width, 1.0f, half_height);
                    SelectionBand(glow, PanelCentreX, cy, half_width, 0.5f, half_height);
                }
                constexpr Rgb Unselected{176.0f / 255, 176.0f / 255, 172.0f / 255};
                const float label_y = oy + (has_detail ? 27.0f : LabelDy);
                DrawText(page, font, r.label, PanelCentreX, label_y, Align::Centre,
                         !r.enabled ? TabDim : on ? Gilt : Unselected, 1.f, size);
                if (has_detail) {
                    DrawText(page, font, r.value, PanelCentreX, oy + 46.0f, Align::Centre,
                             r.enabled ? Unselected : TabDim, 1.0f, 18.0f);
                }
                continue;
            }
            DrawText(page, font, r.label, PanelCentreX, oy + LabelDy, Align::Centre, ink, 1.f,
                     size);
            if (r.selected && r.enabled)
                DrawPieces(glow, assets, PageArt::RowGlow, ox, oy);
            continue;
        }
        DrawText(page, font, r.label, LabelRight, oy + LabelDy, Align::Right, ink);
        if (r.widget == Widget::Slider) {
            DrawPieces(page, assets, PageArt::SliderBox, ox, oy);
            DrawPieces(page, assets, PageArt::SliderTrack, ox, oy);
            const float fill_end = PageArt::SliderFill[0].x0 +
                                   PageArt::SliderFillWidth * std::clamp(r.fraction, 0.0f, 1.0f);
            for (const Piece& p : PageArt::SliderFill) {
                DrawPiece(page, assets, p, ox, oy, false, 1.0f, fill_end);
            }
            DrawText(page, font, std::to_string(r.number), NumberCentreX, oy + NumberDy,
                     Align::Centre, ink, 1.0f, 23.0f);
            if (r.selected && r.enabled) {
                DrawPieces(glow, assets, PageArt::SliderGlow, ox, oy);
                DrawPieces(glow, assets, PageArt::SliderArrowLeft, ox, oy);
                DrawPieces(glow, assets, PageArt::SliderArrowRight, ox, oy);
            }
        } else {
            DrawPieces(page, assets, PageArt::ComboOrnament, ox, oy);
            const float value_width = TextWidth(font, r.value);
            const float value_size = value_width > ValueRoom
                                         ? std::max(16.0f, 24.0f * ValueRoom / value_width)
                                         : 24.0f;
            DrawText(page, font, r.value, ValueCentreX, oy + ValueDy, Align::Centre, ink, 1.0f,
                     value_size);
            if (r.selected && r.enabled) {
                DrawPieces(glow, assets, PageArt::ComboGlow, ox, oy);
                DrawPieces(glow, assets, PageArt::ComboArrowLeft, ox, oy);
                DrawPieces(glow, assets, PageArt::ComboArrowRight, ox, oy);
            }
        }
        if (r.selected && r.enabled) {
            DrawPieces(glow, assets, PageArt::RowGlow, ox, oy);
        }
    }

    // Current rendering state stays separate from the focused setting's help.
    if (!v.graphics_status.empty()) {
        constexpr float room = StatusRoom;
        const float width = TextWidth(font, v.graphics_status);
        const float size = width > room ? std::clamp(24.0f * room / width, 18.0f, 20.0f) : 20.0f;
        DrawText(page, font, v.graphics_status, PanelCentreX, GraphicsStatusBaseline, Align::Centre,
                 HelpKhaki, 1.0f, size);
    }

    // 5. The button row (the right button at the movie's place, the left one mirrored).
    for (int b = 0; b < 2; ++b) {
        const ButtonView& bv = v.buttons[static_cast<size_t>(b)];
        if (!bv.present) {
            continue;
        }
        const bool mirror = b == 0;
        DrawPieces(page, assets, PageArt::ButtonOrnament, PageArt::ButtonX, ButtonOriginY(v),
                   mirror);
        const bool framed = bv.label == "ACCEPT" || bv.label == "SAVE AND RESTART" ||
                            bv.label == "Soft save" || bv.label == "Clear soft save";
        if (framed) {
            const float x0 = Canvas::ToX(ButtonCentreX(b) - ButtonHalfWidth(bv));
            const float x1 = Canvas::ToX(ButtonCentreX(b) + ButtonHalfWidth(bv));
            const float y0 = Canvas::ToY(ButtonOriginY(v) - 36.0f);
            const float y1 = Canvas::ToY(ButtonOriginY(v) + 14.0f);
            const auto border = RoundRect(x0, y0, x1, y1, 4.0f * PageScale);
            FillRoundRect(page, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (x1 - x0) * 0.5f,
                          (y1 - y0) * 0.5f, 4.0f * PageScale,
                          bv.selected ? Rgb{0.22f, 0.17f, 0.08f} : Rgb{0.06f, 0.05f, 0.03f}, 0.95f);
            Stroke(page, border, true, (bv.selected ? 2.4f : 1.4f) * PageScale,
                   bv.selected ? Gilt : TitleGold, 1.0f);
            if (bv.selected)
                Stroke(glow, border, true, 3.5f * PageScale, Gilt, 0.65f);
        }
        // Border spans -36..+14: its vertical centre is -11 from the button origin.
        DrawText(page, font, bv.label, ButtonCentreX(b),
                 ButtonOriginY(v) + (framed ? -11.0f : ButtonTextDy), Align::Centre,
                 bv.selected ? LabelSelected : LabelGrey, 1.0f, 24.0f, 0.0f, false, framed);
        if (bv.selected) {
            DrawPieces(glow, assets, PageArt::ButtonGlow, PageArt::ButtonX, ButtonOriginY(v),
                       mirror);
        }
    }

    // 6. The help and the key guide (glyph, "：", text; letter spacing 2). A help line wider than
    //    the frame's rules goes on two lines (as Show VR controls'), the same size (2026-10-30: 17
    //    of the page's helps were drawn smaller, 5 of them past the frame even so); two still too
    //    wide (a long notice, a long translation of the font) are drawn smaller to fit inside them.
    {
        const auto lines = WrapHelp(font, v.help, HelpRoom);
        float widest = 0.0f;
        for (const auto& l : lines) {
            widest = std::max(widest, TextWidth(font, l));
        }
        const float size = widest > HelpRoom ? std::max(24.0f * HelpRoom / widest, 17.0f) : 24.0f;
        for (size_t i = 0; i < lines.size(); ++i) {
            const float up = HelpLinePitch * static_cast<float>(lines.size() - 1 - i);
            DrawText(page, font, lines[i], HelpCentreX, HelpBaseline - up, Align::Centre,
                     HelpKhaki, 1.0f, size);
        }
    }
    DrawKeyGuide(page, assets, v.keys, v.glyphs);

    // 7. The pointer's halo.
    if (v.hover.target != Target::None && v.hover.target != Target::Page) {
        for (const HitBox& b : HitBoxes(v, font)) {
            if (b.hover != v.hover) {
                continue;
            }
            const bool round = b.hover.target == Target::RowLeft ||
                               b.hover.target == Target::RowRight ||
                               b.hover.target == Target::TabPrev || b.hover.target == Target::TabNext;
            if (round) {
                const float cx = 0.5f * (b.x0 + b.x1), cy = 0.5f * (b.y0 + b.y1);
                const float rr = 15.0f / PageScale; // the mock's ring: 15 image px
                Halo(page, cx - rr, cy - rr, cx + rr, cy + rr, true);
            } else {
                Halo(page, b.x0, b.y0 + 4.0f, b.x1, b.y1 - 2.0f, false);
            }
            break;
        }
    }

    page.ToImage(page_out);
    glow.ToImage(glow_out);
    return true;
}

void ToCompositorAlpha(Image& image) {
    static const std::array<float, 256> decode = [] {
        std::array<float, 256> t{};
        for (size_t i = 0; i < t.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    u8* p = image.rgba.data();
    for (const u8* end = p + (image.rgba.size() & ~size_t{3}); p != end; p += 4) {
        const int a = p[3];
        if (a == 0 || a == 255) {
            continue;
        }
        // The game's result over white is min(k + (1 - a), 1) per channel; the compositor's is
        // G(k) + (1 - alpha') * 1. Over black both are k.
        const int t = 255 - a;
        const float transmit = 0.2126f * (decode[std::min(p[0] + t, 255)] - decode[p[0]]) +
                               0.7152f * (decode[std::min(p[1] + t, 255)] - decode[p[1]]) +
                               0.0722f * (decode[std::min(p[2] + t, 255)] - decode[p[2]]);
        p[3] = static_cast<u8>(std::clamp(1.0f - transmit, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

std::vector<std::string> TextFitReport(const View& v, const GameText::GameFont* font) {
    std::vector<std::string> out;
    if (font == nullptr) {
        return out;
    }
    // `w` drawn at full size up to `room`; smaller (the page's own fit) up to `smaller_room`.
    const auto check = [&](const char* what, const std::string& text, float w, float room,
                           float smaller_room) {
        if (text.empty() || w <= room) {
            return;
        }
        const bool fits = w <= smaller_room;
        out.push_back(std::string{what} + " '" + text + "' " +
                      std::to_string(static_cast<int>(std::lround(w))) + " of " +
                      std::to_string(static_cast<int>(std::lround(room))) + " px: " +
                      (fits ? "drawn smaller" : "OVER"));
    };
    // The headers: left-aligned, before the column divider.
    for (const auto& h : v.headers) {
        check("header", h, TextWidth(font, h), PanelCentreX - 12.0f - HeaderX, 0.0f);
    }
    for (const RowView& r : v.rows) {
        if (!r.present) {
            continue;
        }
        if (r.widget == Widget::Action) {
            check("label", r.label, TextWidth(font, r.label), ActionRoom, ActionRoom * 24.f / 17.f);
            continue;
        }
        // A label: right-aligned on the label column, from the headers' left edge.
        check("label", r.label, TextWidth(font, r.label), LabelRight - HeaderX, 0.0f);
        if (r.widget != Widget::Slider) {
            check("value", r.value, TextWidth(font, r.value), ValueRoom, ValueRoom * 24.f / 16.f);
        }
    }
    // The help: one line or two at full size (Render), else smaller.
    for (const auto& l : WrapHelp(font, v.help, HelpRoom)) {
        check("help line", l, TextWidth(font, l), HelpRoom, HelpRoom * 24.f / 17.f);
    }
    // The rendering state: 20 px up to its room, then down to 18 px (widths at 24 px).
    check("status", v.graphics_status, TextWidth(font, v.graphics_status), StatusRoom * 24.f / 20.f,
          StatusRoom * 24.f / 18.f);
    for (const ButtonView& b : v.buttons) {
        if (b.present) {
            check("button", b.label, TextWidth(font, b.label), 2.0f * ButtonHalfWidth(b) - 24.0f,
                  0.0f);
        }
    }
    // The sub-tab strip: inside the frame's rules.
    if (v.pages > 1 && !v.subtabs.empty()) {
        const StripLayout l = LayoutStrip(v, font);
        std::string names;
        for (const auto& n : v.subtabs) {
            names += (names.empty() ? "" : " | ") + n;
        }
        check("strip", names, l.x.back() + l.width.back() - l.x.front(),
              PageArt::FrameX1 - PageArt::FrameX0 - 160.0f, 0.0f);
    }
    return out;
}

} // namespace VR::Page
