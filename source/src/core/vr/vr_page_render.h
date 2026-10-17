// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/vr/vr_page.h"

namespace VR::GameText {
struct GameFont;
}

// The VR settings page drawn on the CPU (vr_menu=1): a pixel clone of the game's Environment
// settings page (game asset, baked in vr_page_art.h) with the game's own textures
// (game asset) and font (game asset), plus our tab strip, the controller's
// own button badges and the pointer's halo. Two images of the same size: the page, and the
// selection glow (additive, its own layer so the presenter can pulse it like the game's cursor
// without drawing the page again). Pure: no emulator state (the offline tool and the tests render
// it from the game's files directly).
namespace VR::Page {

// Stage px (the game's 1920 x 1080 UI space) covered by the page images.
inline constexpr float PageX0 = 370.0f;
inline constexpr float PageY0 = 140.0f;
inline constexpr float PageX1 = 1610.0f;
inline constexpr float PageY1 = 930.0f;
// Image px per stage px: the menu canvas's 2560 x 1440 UI image (24-px body text = 32 px).
inline constexpr float PageScale = 4.0f / 3.0f;
inline constexpr int PageWidth = 1654;  // ceil((PageX1 - PageX0) * PageScale)
inline constexpr int PageHeight = 1054; // ceil((PageY1 - PageY0) * PageScale)
// The panel's visible centre (stage px): its ornament rules (#33 / #32: x 501.6..1418.1, y
// 151.7..891.7), the title's and the key guide's centre line. The left button mirrors the right
// one about it; the presenter puts it where the menu canvas's centre is.
inline constexpr float PanelCentreX = 959.7f;
inline constexpr float PanelCentreY = 521.7f;

// RGBA8, premultiplied alpha, sRGB-encoded values (the game blends its UI in that domain).
struct Image {
    int width{0};
    int height{0};
    std::vector<u8> rgba;
};

struct Texture {
    int width{0};
    int height{0};
    const u8* rgba{nullptr}; // straight alpha
};

struct Assets {
    Texture common3; // MENU_Common_00003 (frames, ornaments, glows, arrows)
    Texture common1; // MENU_Common_00001 (the slider's bar)
    Texture dpad_lr; // KG_L_LR (key guide: change)
    Texture dpad_ud; // KG_L_UD (key guide: select)
    const GameText::GameFont* font{nullptr};
    [[nodiscard]] bool Ready() const {
        return common3.rgba != nullptr && common1.rgba != nullptr && dpad_lr.rgba != nullptr &&
               dpad_ud.rgba != nullptr && font != nullptr;
    }
};

// The controller's own names for the page's buttons (the Quest's A / B by default): the tabs'
// badges are the grips (LG / RG), the value badges the triggers (LT / RT).
struct Glyphs {
    std::string confirm{"A"};
    std::string back{"B"};
    std::string prev{"LG"};       // the previous tab: the left grip
    std::string next{"RG"};       // the next tab: the right grip
    std::string value_prev{"LT"}; // the previous value: the left trigger
    std::string value_next{"RT"}; // the next value: the right trigger
    bool operator==(const Glyphs&) const = default;
};

// DpadLRValues: the D-pad's left / right glyph and the value badges (LT RT), one item ("Change").
enum class KeyGlyph : unsigned char {
    DpadLR,
    DpadUD,
    Confirm,
    Back,
    Prev,
    Next,
    PrevNext,
    DpadLRValues,
};

struct KeyItem {
    KeyGlyph glyph;
    std::string text;
};

struct RowView {
    bool present{false};
    std::string label;
    std::string value;
    Widget widget{Widget::Choice};
    int number{0};
    float fraction{0.0f}; // Slider
    bool selected{false};
    bool enabled{true};
};

struct ButtonView {
    bool present{false};
    std::string label;
    bool selected{false};
};

struct View {
    std::vector<std::string> tabs;
    int tab{0};
    std::array<std::string, 2> headers{};
    std::array<RowView, Slots> rows{};
    std::array<ButtonView, 2> buttons{};
    std::string help;
    std::string graphics_status;
    std::vector<KeyItem> keys;
    Glyphs glyphs{};
    Hover hover{};   // the pointer's halo
    bool confirm{false};
    // A page of centred rows (the lantern's): the frame without the settings page's column
    // divider and the panel texture's dark gutter beside it (they cut through centred labels).
    bool single_column{false};
};

// The view of `model` now (values read from its store).
View MakeView(Model& model, const Glyphs& glyphs);

// Where the pointer can act, in stage px (hit testing; the halo uses the same boxes).
struct HitBox {
    Hover hover;
    float x0, y0, x1, y1;
};
std::vector<HitBox> HitBoxes(const View& view, const GameText::GameFont* font);
// The element at stage (x, y): the first box containing it, else Page inside the frame, else None.
Hover HitTest(const std::vector<HitBox>& boxes, float x, float y);
// A slider's bar in stage px (x0, x1) for the drag fraction.
std::array<float, 2> SliderBarSpan(int slot);
// The origin (stage y) of the row in `slot`: the movie's Item_N_0 place moved down to give the
// group headers room (everything of a row is placed from it: art, label, glow, hit boxes).
float RowTop(int slot);
// The baselines (stage px) of group header `group` (0: above slot 0, 1: above slot 2) and of the
// label of the row in `slot`.
float HeaderBaseline(int group);
float LabelBaseline(int slot);

// Draws the page and its glow layer (both PageWidth x PageHeight). False without assets.
bool Render(const Assets& assets, const View& view, Image& page, Image& glow);

// An image as the compositor blends it. The game blends its UI in its encoded (sRGB) domain; an
// OpenXR compositor blends an sRGB swapchain in linear light, where the page's 88% black panel
// would let three times as much of the world and of the grid behind it through (the grid's
// numbers and key guide showed behind the button row). The alpha is refitted like the game's own
// UI layer's (renderer_vulkan's vr_ui_resolve.comp, gamma 1): exact over a black and a white
// world, for opaque pixels, text and black dims: 1 - alpha = luma(G(min(k + 1 - a, 1)) - G(k)),
// G the sRGB decode, k the premultiplied colour, whose bytes stay. Pixels without alpha (the
// glow's additive light, empty space) and opaque ones are left as they are. In place; only for
// the swapchain (Render's images stay in the game's domain for the offline tools and tests).
void ToCompositorAlpha(Image& image);

} // namespace VR::Page
