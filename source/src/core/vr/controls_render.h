// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/vr/controls_view.h"
#include "core/vr/vr_page_render.h"

// "Show VR controls" drawn on the CPU (vr_menu=1): the VR page's frame (the game's settings page
// with its middle cleared, as the game's Brightness page) with the tab strip in the title line,
// both controllers of the family in your hands in pale-ink line art, the callouts with their
// leader lines, the help line and the key guide. Two images of the page's size, as the settings
// page: the page, and the selection glow (its own pulsing layer). Pure (the offline tool renders
// it from the game's files).
namespace VR::ControlsView {

struct View {
    std::vector<std::string> tabs;
    int tab{0};
    Family family{Family::Touch};
    std::vector<DrawnPart> parts;
    std::vector<Callout> callouts;
    std::vector<Lit> lights;
    int focus{-1};
    int hover{-1};       // the pointer on a callout (its halo)
    int hover_tab{-1};   // ... on a tab (0..), or the grips' LG / RG badge (-2 / -3)
    std::string help;
    std::string note;
    std::vector<Page::KeyItem> keys;
    Page::Glyphs glyphs{};
};

View MakeView(const Model& model, const Page::Glyphs& glyphs);

// Where the pointer can act (stage px).
struct HitBox {
    enum class Kind : unsigned char { Tab, Prev, Next, Callout } kind;
    int index;
    float x0, y0, x1, y1;
};
std::vector<HitBox> HitBoxes(const View& view, const GameText::GameFont* font);
// The box under stage (x, y), or nullptr.
const HitBox* HitTest(const std::vector<HitBox>& boxes, float x, float y);

// Draws the view and its glow layer (Page::PageWidth x PageHeight). False without assets.
bool Render(const Page::Assets& assets, const View& view, Page::Image& page, Page::Image& glow);

} // namespace VR::ControlsView
