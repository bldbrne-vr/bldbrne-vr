// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/vr/game_font.h"
#include "core/vr/vr_page_art.h"
#include "core/vr/vr_page_render.h"

// The drawing primitives of the VR pages (vr_menu=1), shared by the settings page
// (vr_page_render.cpp) and the controls view (controls_render.cpp): a float canvas in the page
// image's pixels, the game's baked art pieces, the game's font, the controller's button badges,
// the key guide and the tab strip in the title line, the pointer's halo, and pale-ink vector
// drawing (polygons, strokes, soft glows, hatching) for the controllers' line art. Header-only
// and pure (no emulator state).
namespace VR::Page::Draw {

using PageArt::Atlas;
using PageArt::Blend;
using PageArt::Piece;

// ---- Layout (stage px), from the game's Environment page (game asset) -------------------

inline constexpr float Ascent24 = 21.1171875f;  // the menu font's ascent at 24 px (18020 / 20480 em)
inline constexpr float TitleCentreX = 959.6f;   // the page title's field (#104 / StaticText_111020)
inline constexpr float TitleBaseline = 226.72f; // 204.7 - 1.1 + 2 + ascent
inline constexpr float HelpCentreX = 961.0f;    // StatusBar/LineHelp
inline constexpr float HelpBaseline = 791.98f;
inline constexpr float KeyCentreX = 959.8f; // StatusBar/KeyGuide (letterSpacing 2)
inline constexpr float KeyTop = 805.8f;
inline constexpr float KeyBaseline = 805.8f + Ascent24;
inline constexpr float KeySpacing = 0.0f; // the edit text says 2; the game's key guide is drawn with 0
inline constexpr float MirrorX = PanelCentreX; // the panel's centre line
// The tab strip in the title line (ours): the gaps between tabs and to the LT / RT badges. Six
// tabs or more (Graphics, 2026-10-07) close up to TabGapTight, so the strip stays inside the
// frame's rule lines (with the menu font: 528 .. 1391 stage px, five tabs 576 .. 1343).
inline constexpr float TabGap = 48.0f;
inline constexpr float TabGapTight = 36.0f;
inline constexpr float TabBadgeGap = 45.0f;
inline constexpr float TabGapFor(std::size_t tabs) {
    return tabs > 5 ? TabGapTight : TabGap;
}

// Colours (the movie's text colours; the selected row's from the game's pages).
struct Rgb {
    float r, g, b;
};
inline constexpr Rgb TitleGold{160.0f / 255, 130.0f / 255, 75.0f / 255};
inline constexpr Rgb TabDim{112.0f / 255, 108.0f / 255, 98.0f / 255};
inline constexpr Rgb LabelGrey{204.0f / 255, 204.0f / 255, 204.0f / 255};
inline constexpr Rgb LabelSelected{255.0f / 255, 252.0f / 255, 228.0f / 255};
inline constexpr Rgb HeaderTeal{189.0f / 255, 214.0f / 255, 214.0f / 255};
inline constexpr Rgb HelpKhaki{165.0f / 255, 165.0f / 255, 140.0f / 255};
inline constexpr Rgb Gilt{255.0f / 255, 226.0f / 255, 170.0f / 255};

// A float RGBA canvas (premultiplied), stage px -> image px through PageX0 / PageY0 / PageScale.
struct Canvas {
    int w{PageWidth};
    int h{PageHeight};
    std::vector<float> px; // r g b a

    Canvas() : px(static_cast<size_t>(PageWidth) * PageHeight * 4, 0.0f) {}

    static float ToX(float stage_x) {
        return (stage_x - PageX0) * PageScale;
    }
    static float ToY(float stage_y) {
        return (stage_y - PageY0) * PageScale;
    }
    static float StageX(float image_x) {
        return PageX0 + image_x / PageScale;
    }
    static float StageY(float image_y) {
        return PageY0 + image_y / PageScale;
    }

    void Over(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) {
            return;
        }
        float* d = &px[(static_cast<size_t>(y) * w + x) * 4];
        const float k = 1.0f - a;
        d[0] = r * a + d[0] * k;
        d[1] = g * a + d[1] * k;
        d[2] = b * a + d[2] * k;
        d[3] = a + d[3] * k;
    }
    void Add(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) {
            return;
        }
        float* d = &px[(static_cast<size_t>(y) * w + x) * 4];
        d[0] = std::min(1.0f, d[0] + r * a);
        d[1] = std::min(1.0f, d[1] + g * a);
        d[2] = std::min(1.0f, d[2] + b * a);
    }

    void ToImage(Image& out) const {
        out.width = w;
        out.height = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < out.rgba.size(); ++i) {
            out.rgba[i] = static_cast<u8>(std::clamp(px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
};

inline const Texture& AtlasOf(const Assets& a, Atlas atlas) {
    return atlas == Atlas::Common1 ? a.common1 : a.common3;
}

// The screen dim under the panel in VR, for a texel of the panel's footprint with alpha `t`: the
// game's dim (`dim`, 60%) over the soft edges, rising to full where the panel is solid, so the
// panel's middle is opaque (the world never shows through the text's backing: the play test of
// 2026-10-09) while its soft edges keep the game's look (t <= 0.6: within 0.7% of dim * t; 83%
// of the rise is in the panel's innermost texels, t > 0.8). 1 at t = 1.
inline float PanelDim(float t, float dim) {
    const float t2 = t * t;
    const float t4 = t2 * t2;
    return dim * t + (1.0f - dim) * t4 * t4;
}

// PanelDim under the page's and the controls view's panel: on unless VR_PAGE_SEE_THROUGH is set
// (the game's dim everywhere: its 88% panel, 12% of the world through).
inline bool PanelOpaque() {
    static const bool opaque = std::getenv("VR_PAGE_SEE_THROUGH") == nullptr;
    return opaque;
}

// One baked piece at `origin` (stage px). `mirror`: reflected about MirrorX (the left button).
// `clip_x1`: the slider fill's value edge (stage px, local; +inf = none). `alpha`: times the
// piece's own.
// `shadow`: the piece's footprint only (its texels' alpha) in black at `alpha` (the screen dim).
// `opaque_core` (with `shadow`): PanelDim, the dim full where the piece is solid.
inline void DrawPiece(Canvas& c, const Assets& assets, const Piece& p, float ox, float oy,
                      bool mirror, float alpha, float clip_x1 = 1e9f, bool shadow = false,
                      bool opaque_core = false) {
    float x0 = p.x0, x1 = std::min(p.x1, clip_x1);
    if (x1 <= x0) {
        return;
    }
    float sx0 = ox + x0, sx1 = ox + x1;
    if (mirror) {
        const float a = 2.0f * MirrorX - sx1, b = 2.0f * MirrorX - sx0;
        sx0 = a;
        sx1 = b;
    }
    const float sy0 = oy + p.y0, sy1 = oy + p.y1;
    const int X0 = std::max(0, static_cast<int>(std::floor(Canvas::ToX(sx0))));
    const int X1 = std::min(c.w, static_cast<int>(std::ceil(Canvas::ToX(sx1))));
    const int Y0 = std::max(0, static_cast<int>(std::floor(Canvas::ToY(sy0))));
    const int Y1 = std::min(c.h, static_cast<int>(std::ceil(Canvas::ToY(sy1))));
    const Texture* tex = p.atlas == Atlas::Solid ? nullptr : &AtlasOf(assets, p.atlas);
    if (tex != nullptr && tex->rgba == nullptr) {
        return;
    }
    const float a_piece = shadow ? alpha : p.alpha * alpha;
    const float inv = 1.0f / PageScale;
    for (int y = Y0; y < Y1; ++y) {
        const float py0 = PageY0 + y * inv, py1 = py0 + inv;
        const float cy = std::clamp((std::min(py1, sy1) - std::max(py0, sy0)) * PageScale, 0.0f,
                                    1.0f);
        if (cy <= 0.0f) {
            continue;
        }
        const float ly = py0 + 0.5f * inv - oy; // local stage y of the pixel's centre
        for (int x = X0; x < X1; ++x) {
            const float px0 = PageX0 + x * inv, px1 = px0 + inv;
            const float cx = std::clamp((std::min(px1, sx1) - std::max(px0, sx0)) * PageScale,
                                        0.0f, 1.0f);
            if (cx <= 0.0f) {
                continue;
            }
            const float cov = cx * cy * a_piece;
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            if (tex != nullptr) {
                float lx = px0 + 0.5f * inv - ox;
                if (mirror) {
                    lx = 2.0f * MirrorX - (px0 + 0.5f * inv) - ox;
                }
                const float u = p.u0 + p.ux * lx + p.uy * ly;
                const float v = p.v0 + p.vx * lx + p.vy * ly;
                int iu = static_cast<int>(std::floor(u));
                int iv = static_cast<int>(std::floor(v));
                if (p.repeat) {
                    iu = ((iu % tex->width) + tex->width) % tex->width;
                    iv = ((iv % tex->height) + tex->height) % tex->height;
                } else {
                    iu = std::clamp(iu, 0, tex->width - 1);
                    iv = std::clamp(iv, 0, tex->height - 1);
                }
                const u8* t = tex->rgba + (static_cast<size_t>(iv) * tex->width + iu) * 4;
                r = t[0] / 255.0f;
                g = t[1] / 255.0f;
                b = t[2] / 255.0f;
                a = t[3] / 255.0f;
            }
            if (shadow) {
                c.Over(x, y, 0.0f, 0.0f, 0.0f,
                       opaque_core ? cx * cy * PanelDim(a, a_piece) : a * cov);
            } else if (p.blend == Blend::Add) {
                c.Add(x, y, r, g, b, a * cov);
            } else {
                c.Over(x, y, r, g, b, a * cov);
            }
        }
    }
}

template <size_t N>
void DrawPieces(Canvas& c, const Assets& a, const Piece (&pieces)[N], float ox, float oy,
                bool mirror = false, float alpha = 1.0f) {
    for (const Piece& p : pieces) {
        DrawPiece(c, a, p, ox, oy, mirror, alpha);
    }
}

// ---- Text ----

inline float TextWidth(const GameText::GameFont* font, const std::string& text,
                       float size_stage = 24.0f, float spacing_stage = 0.0f) {
    if (font == nullptr || text.empty()) {
        return 0.0f;
    }
    return GameText::LineAdvance(*font, text.c_str(), size_stage * PageScale,
                                 spacing_stage * PageScale) /
           PageScale;
}

enum class Align { Left, Centre, Right };

// Draws `text` with its pen at stage x (aligned) and its baseline at stage y. Returns the stage
// width. With centre_vertical, baseline instead specifies the visible ink centre.
inline float DrawText(Canvas& c, const GameText::GameFont* font, const std::string& text, float x,
                      float baseline, Align align, Rgb color, float alpha = 1.0f,
                      float size = 24.0f, float spacing = 0.0f, bool additive = false,
                      bool centre_vertical = false) {
    if (font == nullptr || text.empty()) {
        return 0.0f;
    }
    GameText::Line line;
    constexpr u32 Pad = 2;
    if (!GameText::RasterizeLine(*font, text.c_str(), size * PageScale, spacing * PageScale, Pad,
                                 line)) {
        return 0.0f;
    }
    const float width = line.advance / PageScale;
    const float pen = align == Align::Left    ? x
                      : align == Align::Right ? x - width
                                              : x - 0.5f * width;
    const int ox = static_cast<int>(std::lround(Canvas::ToX(pen))) - static_cast<int>(Pad);
    float vertical_origin = Canvas::ToY(baseline) - line.baseline;
    if (centre_vertical) {
        // Centre the actual letters, not the font's unused ascender/descender space.
        u32 top = line.height, bottom = 0;
        for (u32 y = 0; y < line.height; ++y) {
            const auto row = line.cover.begin() + size_t(y) * line.width;
            if (std::any_of(row, row + line.width, [](u8 coverage) { return coverage != 0; })) {
                top = std::min(top, y);
                bottom = y + 1;
            }
        }
        if (top < bottom)
            vertical_origin = Canvas::ToY(baseline) - 0.5f * float(top + bottom);
    }
    const int oy = static_cast<int>(std::lround(vertical_origin));
    for (u32 y = 0; y < line.height; ++y) {
        for (u32 xx = 0; xx < line.width; ++xx) {
            const u8 v = line.cover[static_cast<size_t>(y) * line.width + xx];
            if (v == 0) {
                continue;
            }
            const float a = v / 255.0f * alpha;
            if (additive) {
                c.Add(ox + static_cast<int>(xx), oy + static_cast<int>(y), color.r, color.g,
                      color.b, a);
            } else {
                c.Over(ox + static_cast<int>(xx), oy + static_cast<int>(y), color.r, color.g,
                       color.b, a);
            }
        }
    }
    return width;
}

// ---- Procedural shapes (badges, halo): signed distances, 1 px anti-aliasing, image px ----

inline float Coverage(float d) {
    return std::clamp(0.5f - d, 0.0f, 1.0f); // d: signed distance in image px (inside < 0)
}

inline float RoundRectDistance(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - (hw - r);
    const float qy = std::abs(y - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::hypot(ox, oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// A filled rounded rect (image px), colour (0..255) and alpha.
inline void FillRoundRect(Canvas& c, float cx, float cy, float hw, float hh, float r, Rgb col,
                          float a) {
    const int X0 = static_cast<int>(std::floor(cx - hw - 1)),
              X1 = static_cast<int>(std::ceil(cx + hw + 1));
    const int Y0 = static_cast<int>(std::floor(cy - hh - 1)),
              Y1 = static_cast<int>(std::ceil(cy + hh + 1));
    for (int y = Y0; y < Y1; ++y) {
        for (int x = X0; x < X1; ++x) {
            const float cov = Coverage(
                RoundRectDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, std::min({r, hw, hh})));
            if (cov > 0.0f) {
                c.Over(x, y, col.r, col.g, col.b, a * cov);
            }
        }
    }
}

// The upper rim highlight of a badge: an arc of the outline over its top (200..340 degrees).
inline void Rim(Canvas& c, float cx, float cy, float hw, float hh, float r, Rgb col, float a,
                float width) {
    const int X0 = static_cast<int>(std::floor(cx - hw - 2)),
              X1 = static_cast<int>(std::ceil(cx + hw + 2));
    const int Y0 = static_cast<int>(std::floor(cy - hh - 2)), Y1 = static_cast<int>(std::ceil(cy));
    for (int y = Y0; y < Y1; ++y) {
        for (int x = X0; x < X1; ++x) {
            const float d =
                RoundRectDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, std::min({r, hw, hh}));
            const float cov = Coverage(std::abs(d + 0.5f * width) - 0.5f * width);
            // fade towards the sides (the arc ends)
            const float side = std::clamp(1.0f - std::abs(x + 0.5f - cx) / (hw + 1.0f), 0.0f, 1.0f);
            const float k = std::clamp((cy - (y + 0.5f)) / std::max(hh * 0.35f, 1.0f), 0.0f, 1.0f);
            if (cov > 0.0f) {
                c.Over(x, y, col.r, col.g, col.b, a * cov * std::min(1.0f, side * 2.5f) * k);
            }
        }
    }
}

// A controller button badge (the key guide's style): a dark disc (one letter) or pill, a lighter
// rim on top, the letters in the game's font. Centre in stage px; `height` in stage px.
inline void Badge(Canvas& c, const GameText::GameFont* font, const std::string& text, float sx,
                  float sy, bool trigger) {
    const float cx = Canvas::ToX(sx), cy = Canvas::ToY(sy);
    const float text_px = trigger ? 20.0f : 24.0f; // image px (the mock's)
    const float tw =
        font != nullptr ? GameText::LineAdvance(*font, text.c_str(), text_px,
                                                trigger ? 0.04f * text_px : 0.0f)
                        : 0.0f;
    float hw, hh, r;
    if (trigger) {
        hh = 15.0f;
        hw = std::max(25.0f, 0.5f * tw + 10.0f);
        r = 10.0f;
    } else {
        hh = 17.0f;
        hw = std::max(17.0f, 0.5f * tw + 9.0f);
        r = 17.0f;
    }
    FillRoundRect(c, cx, cy, hw + 2.0f, hh + 2.0f, r + 2.0f, Rgb{8 / 255.0f, 8 / 255.0f, 9 / 255.0f},
                  200.0f / 255.0f);
    FillRoundRect(c, cx, cy, hw, hh, r, Rgb{58 / 255.0f, 58 / 255.0f, 62 / 255.0f}, 1.0f);
    FillRoundRect(c, cx, cy + 1.0f, hw - 2.0f, hh - 2.0f, r - 2.0f,
                  Rgb{34 / 255.0f, 34 / 255.0f, 37 / 255.0f}, 1.0f);
    Rim(c, cx, cy, hw, hh, r, Rgb{120 / 255.0f, 120 / 255.0f, 126 / 255.0f}, 1.0f, 1.6f);
    // the letters (image px -> stage px for DrawText)
    const Rgb ink = trigger ? Rgb{214 / 255.0f, 214 / 255.0f, 218 / 255.0f}
                            : Rgb{222 / 255.0f, 222 / 255.0f, 226 / 255.0f};
    DrawText(c, font, text, sx, sy + (trigger ? 7.5f : 8.5f) / PageScale, Align::Centre, ink, 1.0f,
             text_px / PageScale, trigger ? 0.04f * text_px / PageScale : 0.0f);
}

// The width (stage px) a badge takes.
inline float BadgeWidth(const GameText::GameFont* font, const std::string& text, bool trigger) {
    const float text_px = trigger ? 20.0f : 24.0f;
    const float tw = font != nullptr ? GameText::LineAdvance(*font, text.c_str(), text_px,
                                                             trigger ? 0.04f * text_px : 0.0f)
                                     : 0.0f;
    const float hw =
        trigger ? std::max(25.0f, 0.5f * tw + 10.0f) : std::max(17.0f, 0.5f * tw + 9.0f);
    return 2.0f * (hw + 2.0f) / PageScale;
}

// A key-guide glyph texture (32 x 32 stage px, bilinear) with its top-left at stage (x, y).
inline void DrawGlyph(Canvas& c, const Texture& t, float sx, float sy, float size = 32.0f) {
    if (t.rgba == nullptr) {
        return;
    }
    const float x0 = Canvas::ToX(sx), y0 = Canvas::ToY(sy), s = size * PageScale;
    for (int y = static_cast<int>(std::floor(y0)); y < static_cast<int>(std::ceil(y0 + s)); ++y) {
        for (int x = static_cast<int>(std::floor(x0)); x < static_cast<int>(std::ceil(x0 + s));
             ++x) {
            const float u = (x + 0.5f - x0) / s * t.width - 0.5f;
            const float v = (y + 0.5f - y0) / s * t.height - 0.5f;
            if (u < -0.5f || v < -0.5f || u > t.width - 0.5f || v > t.height - 0.5f) {
                continue;
            }
            const int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
            const float fu = u - iu, fv = v - iv;
            float acc[4]{};
            for (int k = 0; k < 4; ++k) {
                const int tu = std::clamp(iu + (k & 1), 0, t.width - 1);
                const int tv = std::clamp(iv + (k >> 1), 0, t.height - 1);
                const float w = ((k & 1) ? fu : 1.0f - fu) * ((k >> 1) ? fv : 1.0f - fv);
                const u8* p = t.rgba + (static_cast<size_t>(tv) * t.width + tu) * 4;
                const float a = p[3] / 255.0f;
                acc[0] += p[0] / 255.0f * a * w;
                acc[1] += p[1] / 255.0f * a * w;
                acc[2] += p[2] / 255.0f * a * w;
                acc[3] += a * w;
            }
            if (acc[3] > 0.0f) {
                c.Over(x, y, acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3]);
            }
        }
    }
}

// The pointer's halo: a thin gilt ring (circle or rounded box) around what it is on, with a soft
// outer glow (no beam, no dot).
inline void Halo(Canvas& c, float sx0, float sy0, float sx1, float sy1, bool round) {
    const float cx = Canvas::ToX(0.5f * (sx0 + sx1)), cy = Canvas::ToY(0.5f * (sy0 + sy1));
    float hw = 0.5f * (sx1 - sx0) * PageScale, hh = 0.5f * (sy1 - sy0) * PageScale;
    float r = round ? std::max(hw, hh) : 10.0f;
    if (round) {
        hw = hh = r;
    }
    const int X0 = static_cast<int>(std::floor(cx - hw - 10)),
              X1 = static_cast<int>(std::ceil(cx + hw + 10));
    const int Y0 = static_cast<int>(std::floor(cy - hh - 10)),
              Y1 = static_cast<int>(std::ceil(cy + hh + 10));
    for (int y = Y0; y < Y1; ++y) {
        for (int x = X0; x < X1; ++x) {
            const float d =
                RoundRectDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, std::min({r, hw, hh}));
            const float ring = Coverage(std::abs(d) - 0.9f);                      // ~1.8 px line
            const float glow = std::exp(-0.5f * (d / 3.2f) * (d / 3.2f)) * 0.35f; // soft halo
            c.Add(x, y, Gilt.r, Gilt.g, Gilt.b, glow);
            c.Over(x, y, 1.0f, 238 / 255.0f, 205 / 255.0f, ring * 0.9f);
        }
    }
}

// ---- The tab strip and the key guide ----

struct TabLayout {
    std::vector<float> x;     // each tab's pen x
    std::vector<float> width; // each tab's width
    float prev_cx{0.0f}, next_cx{0.0f}, badge_cy{0.0f};
    float prev_w{0.0f}, next_w{0.0f};
    float font_size{24.0f};
};

inline TabLayout LayoutTabs(const std::vector<std::string>& tabs, const Glyphs& glyphs,
                            const GameText::GameFont *font, int active = 0) {
    TabLayout t;
    // Keep readable type as settings grow. More than six tabs scroll a five-tab window;
    // drawing and hit testing share this exact layout. Zero width means not visible.
    const int n = static_cast<int>(tabs.size());
    const int count = n > 6 ? 5 : n;
    const int first = std::clamp(active - count / 2, 0, n - count);
    t.width.resize(tabs.size());
    t.x.resize(tabs.size());
    float total = 0.0f;
    for (int i = first; i < first + count; ++i) {
        t.width[i] = TextWidth(font, tabs[i]);
        total += t.width[i];
    }
    const float gap = TabGapFor(count);
    total += gap * static_cast<float>(std::max(count - 1, 0));
    float x = TitleCentreX - 0.5f * total;
    for (int i = first; i < first + count; ++i) {
        t.x[i] = x;
        x += t.width[i] + gap;
    }
    t.prev_w = BadgeWidth(font, glyphs.prev, true);
    t.next_w = BadgeWidth(font, glyphs.next, true);
    t.prev_cx = TitleCentreX - 0.5f * total - TabBadgeGap;
    t.next_cx = TitleCentreX + 0.5f * total + TabBadgeGap;
    t.badge_cy = TitleBaseline - 8.5f;
    return t;
}

// The tabs in the title line: the active one in the title's gold with a gilt hairline under it,
// the grips' badges (LG / RG) either side.
inline void DrawTabStrip(Canvas& page, const GameText::GameFont* font,
                         const std::vector<std::string>& tabs, int active_tab,
                         const Glyphs& glyphs) {
    const TabLayout t = LayoutTabs(tabs, glyphs, font, active_tab);
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (t.width[i] <= 0.0f)
            continue;
        const bool active = static_cast<int>(i) == active_tab;
        DrawText(page, font, tabs[i], t.x[i], TitleBaseline, Align::Left,
                 active ? TitleGold : TabDim, 1.0f, t.font_size);
        if (active) {
            const float y = Canvas::ToY(TitleBaseline + 8.0f);
            for (int x = static_cast<int>(Canvas::ToX(t.x[i] + 4.5f));
                 x < static_cast<int>(Canvas::ToX(t.x[i] + t.width[i] - 4.5f)); ++x) {
                page.Over(x, static_cast<int>(y), 150 / 255.0f, 122 / 255.0f, 70 / 255.0f,
                          200 / 255.0f);
                page.Over(x, static_cast<int>(y) + 1, 150 / 255.0f, 122 / 255.0f, 70 / 255.0f,
                          110 / 255.0f);
            }
        }
    }
    Badge(page, font, glyphs.prev, t.prev_cx, t.badge_cy, true);
    Badge(page, font, glyphs.next, t.next_cx, t.badge_cy, true);
}

// The key guide (glyph, "：", text) centred under the help line.
inline void DrawKeyGuide(Canvas& page, const Assets& assets, const std::vector<KeyItem>& keys,
                         const Glyphs& glyphs) {
    const GameText::GameFont* font = assets.font;
    struct Item {
        const KeyItem* k;
        float glyph_w;
        float text_w;
    };
    std::vector<Item> items;
    float total = 0.0f;
    constexpr float ItemGap = 22.0f;
    constexpr float PairGap = 4.0f; // LT and RT side by side (one item: "Page")
    const auto glyph_width = [&](const KeyItem& k) {
        switch (k.glyph) {
        case KeyGlyph::Confirm:
        case KeyGlyph::Back:
            return BadgeWidth(font, k.glyph == KeyGlyph::Confirm ? glyphs.confirm : glyphs.back,
                              false);
        case KeyGlyph::Prev:
        case KeyGlyph::Next:
            return BadgeWidth(font, k.glyph == KeyGlyph::Prev ? glyphs.prev : glyphs.next, true);
        case KeyGlyph::PrevNext:
            return BadgeWidth(font, glyphs.prev, true) + PairGap +
                   BadgeWidth(font, glyphs.next, true);
        case KeyGlyph::DpadLRValues:
            return 32.0f + PairGap + BadgeWidth(font, glyphs.value_prev, true) + PairGap +
                   BadgeWidth(font, glyphs.value_next, true);
        default:
            return 32.0f;
        }
    };
    for (const KeyItem& k : keys) {
        const std::string text = "\xEF\xBC\x9A" + k.text; // "：" + text
        const float gw = glyph_width(k);
        const float tw = TextWidth(font, text, 24.0f, KeySpacing);
        items.push_back({&k, gw, tw});
        total += gw + tw;
    }
    total += ItemGap * static_cast<float>(std::max<size_t>(items.size(), 1) - 1);
    float x = KeyCentreX - 0.5f * total;
    for (const Item& it : items) {
        switch (it.k->glyph) {
        case KeyGlyph::DpadLR:
            DrawGlyph(page, assets.dpad_lr, x, KeyTop);
            break;
        case KeyGlyph::DpadUD:
            DrawGlyph(page, assets.dpad_ud, x, KeyTop);
            break;
        case KeyGlyph::Confirm:
            Badge(page, font, glyphs.confirm, x + 0.5f * it.glyph_w, KeyTop + 16.0f, false);
            break;
        case KeyGlyph::Back:
            Badge(page, font, glyphs.back, x + 0.5f * it.glyph_w, KeyTop + 16.0f, false);
            break;
        case KeyGlyph::Prev:
            Badge(page, font, glyphs.prev, x + 0.5f * it.glyph_w, KeyTop + 16.0f, true);
            break;
        case KeyGlyph::Next:
            Badge(page, font, glyphs.next, x + 0.5f * it.glyph_w, KeyTop + 16.0f, true);
            break;
        case KeyGlyph::PrevNext: {
            const float wp = BadgeWidth(font, glyphs.prev, true);
            const float wn = BadgeWidth(font, glyphs.next, true);
            Badge(page, font, glyphs.prev, x + 0.5f * wp, KeyTop + 16.0f, true);
            Badge(page, font, glyphs.next, x + wp + PairGap + 0.5f * wn, KeyTop + 16.0f, true);
            break;
        }
        case KeyGlyph::DpadLRValues: {
            DrawGlyph(page, assets.dpad_lr, x, KeyTop);
            const float x0 = x + 32.0f + PairGap;
            const float wp = BadgeWidth(font, glyphs.value_prev, true);
            const float wn = BadgeWidth(font, glyphs.value_next, true);
            Badge(page, font, glyphs.value_prev, x0 + 0.5f * wp, KeyTop + 16.0f, true);
            Badge(page, font, glyphs.value_next, x0 + wp + PairGap + 0.5f * wn, KeyTop + 16.0f,
                  true);
            break;
        }
        }
        x += it.glyph_w;
        DrawText(page, font, "\xEF\xBC\x9A" + it.k->text, x, KeyBaseline, Align::Left, HelpKhaki,
                 1.0f, 24.0f, KeySpacing);
        x += it.text_w + ItemGap;
    }
}

// ---- Pale-ink vector drawing (image px; the controllers' line art, the pictograms) ----------
// Each shape is rasterised into a coverage mask over its own bounds (polygons: the font's
// non-zero scanline fill, 5 rows per pixel with exact horizontal coverage; strokes: per-segment
// distance, the max of the segments so joins never double up), then composited once.

struct Pt {
    float x, y;
};
using Poly = std::vector<Pt>;

// An ellipse (centre, radii, rotated `rot_deg`, `n` points), e.g. the controller's face.
inline Poly Ellipse(float cx, float cy, float rx, float ry, float rot_deg = 0.0f, int n = 72) {
    Poly p;
    p.reserve(static_cast<size_t>(n));
    const float r = rot_deg * 3.14159265f / 180.0f;
    const float cr = std::cos(r), sr = std::sin(r);
    for (int i = 0; i < n; ++i) {
        const float t = 6.28318531f * static_cast<float>(i) / static_cast<float>(n);
        const float ex = rx * std::cos(t), ey = ry * std::sin(t);
        p.push_back({cx + ex * cr - ey * sr, cy + ex * sr + ey * cr});
    }
    return p;
}

// An arc of an ellipse from a0 to a1 degrees (0 = +x, 90 = +y: image y down).
inline Poly Arc(float cx, float cy, float rx, float ry, float a0_deg, float a1_deg, int n = 40) {
    Poly p;
    for (int i = 0; i <= n; ++i) {
        const float t = (a0_deg + (a1_deg - a0_deg) * static_cast<float>(i) / static_cast<float>(n)) *
                        3.14159265f / 180.0f;
        p.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return p;
}

// A rounded rectangle as a polygon (corner radius r).
inline Poly RoundRect(float x0, float y0, float x1, float y1, float r, int corner_n = 8) {
    Poly p;
    const float cs[4][3] = {{x1 - r, y0 + r, -90.0f}, {x1 - r, y1 - r, 0.0f},
                            {x0 + r, y1 - r, 90.0f}, {x0 + r, y0 + r, 180.0f}};
    for (const auto& c : cs) {
        for (int i = 0; i <= corner_n; ++i) {
            const float t = (c[2] + 90.0f * static_cast<float>(i) / static_cast<float>(corner_n)) *
                            3.14159265f / 180.0f;
            p.push_back({c[0] + r * std::cos(t), c[1] + r * std::sin(t)});
        }
    }
    return p;
}

// Scale, then offset; `mirror_w` > 0 mirrors x within a box of that width first.
inline Poly Place(const Poly& in, float ox, float oy, float scale = 1.0f, float mirror_w = 0.0f) {
    Poly p;
    p.reserve(in.size());
    for (const Pt& q : in) {
        const float x = mirror_w > 0.0f ? mirror_w - q.x : q.x;
        p.push_back({ox + x * scale, oy + q.y * scale});
    }
    return p;
}
inline Pt Place(Pt q, float ox, float oy, float scale = 1.0f, float mirror_w = 0.0f) {
    const float x = mirror_w > 0.0f ? mirror_w - q.x : q.x;
    return {ox + x * scale, oy + q.y * scale};
}

inline float SegmentDistance(float px, float py, const Pt& a, const Pt& b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = px - a.x, wy = py - a.y;
    const float l2 = vx * vx + vy * vy;
    const float t = l2 > 1e-9f ? std::clamp((wx * vx + wy * vy) / l2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(wx - t * vx, wy - t * vy);
}

// A coverage mask (0..1) over the image px box x0..x0+w, y0..y0+h.
struct Mask {
    int x0{0}, y0{0}, w{0}, h{0};
    std::vector<float> a;
    float& At(int x, int y) {
        return a[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
    float Get(int x, int y) const {
        return x < 0 || y < 0 || x >= w || y >= h
                   ? 0.0f
                   : a[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
};

inline Mask EmptyMask(const Poly& p, float pad) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Pt& q : p) {
        x0 = std::min(x0, q.x);
        y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x);
        y1 = std::max(y1, q.y);
    }
    Mask m;
    if (p.empty()) {
        return m;
    }
    m.x0 = static_cast<int>(std::floor(x0 - pad));
    m.y0 = static_cast<int>(std::floor(y0 - pad));
    m.w = static_cast<int>(std::ceil(x1 + pad)) - m.x0 + 1;
    m.h = static_cast<int>(std::ceil(y1 + pad)) - m.y0 + 1;
    m.a.assign(static_cast<size_t>(m.w) * static_cast<size_t>(m.h), 0.0f);
    return m;
}

// A closed polygon's coverage (non-zero winding).
inline Mask FillMask(const Poly& p, float pad = 1.0f) {
    Mask m = EmptyMask(p, pad);
    if (p.size() < 3) {
        return m;
    }
    std::vector<GameText::Segment> segs;
    segs.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        const Pt& a = p[i];
        const Pt& b = p[(i + 1) % p.size()];
        segs.push_back({a.x - static_cast<float>(m.x0), a.y - static_cast<float>(m.y0),
                        b.x - static_cast<float>(m.x0), b.y - static_cast<float>(m.y0)});
    }
    GameText::FillOutlines(segs, static_cast<u32>(m.w), static_cast<u32>(m.h), m.a);
    for (float& v : m.a) {
        v = std::min(v, 1.0f);
    }
    return m;
}

// A polyline's stroke coverage (round joins and caps), `width` image px.
inline Mask StrokeMask(const Poly& p, bool closed, float width) {
    const float hw = 0.5f * width;
    Mask m = EmptyMask(p, hw + 2.0f);
    if (p.size() < 2) {
        return m;
    }
    const size_t n = p.size();
    const size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        const Pt& a = p[i];
        const Pt& b = p[(i + 1) % n];
        const int X0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - hw - 1.0f)) - m.x0);
        const int X1 = std::min(m.w, static_cast<int>(std::ceil(std::max(a.x, b.x) + hw + 1.0f)) - m.x0 + 1);
        const int Y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - hw - 1.0f)) - m.y0);
        const int Y1 = std::min(m.h, static_cast<int>(std::ceil(std::max(a.y, b.y) + hw + 1.0f)) - m.y0 + 1);
        for (int y = Y0; y < Y1; ++y) {
            for (int x = X0; x < X1; ++x) {
                const float d = SegmentDistance(static_cast<float>(x + m.x0) + 0.5f,
                                                static_cast<float>(y + m.y0) + 0.5f, a, b);
                const float cov = Coverage(d - hw);
                float& v = m.At(x, y);
                v = std::max(v, cov);
            }
        }
    }
    return m;
}

// A separable Gaussian blur of `sigma` px (the mask grows by 3 sigma on every side).
inline Mask Blur(const Mask& in, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> k(static_cast<size_t>(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) {
        k[static_cast<size_t>(i + r)] = std::exp(-0.5f * (i / sigma) * (i / sigma));
        sum += k[static_cast<size_t>(i + r)];
    }
    for (float& v : k) {
        v /= sum;
    }
    Mask out;
    out.x0 = in.x0 - r;
    out.y0 = in.y0 - r;
    out.w = in.w + 2 * r;
    out.h = in.h + 2 * r;
    std::vector<float> tmp(static_cast<size_t>(out.w) * static_cast<size_t>(in.h), 0.0f);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i) {
                acc += in.Get(x - r + i, y) * k[static_cast<size_t>(i + r)];
            }
            tmp[static_cast<size_t>(y) * static_cast<size_t>(out.w) + static_cast<size_t>(x)] = acc;
        }
    }
    out.a.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h), 0.0f);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i) {
                const int sy = y - r + i;
                if (sy >= 0 && sy < in.h) {
                    acc += tmp[static_cast<size_t>(sy) * static_cast<size_t>(out.w) +
                               static_cast<size_t>(x)] *
                           k[static_cast<size_t>(i + r)];
                }
            }
            out.At(x, y) = acc;
        }
    }
    return out;
}

// What a vector paint does per pixel: Over (ink) or Add (light).
enum class Mode { Over, Add };

inline void Composite(Canvas& c, const Mask& m, Rgb col, float alpha, Mode mode = Mode::Over) {
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            const float v = m.a[static_cast<size_t>(y) * static_cast<size_t>(m.w) +
                                static_cast<size_t>(x)];
            if (v <= 0.0f) {
                continue;
            }
            if (mode == Mode::Add) {
                c.Add(x + m.x0, y + m.y0, col.r, col.g, col.b, alpha * v);
            } else {
                c.Over(x + m.x0, y + m.y0, col.r, col.g, col.b, alpha * v);
            }
        }
    }
}

inline void FillPoly(Canvas& c, const Poly& p, Rgb col, float alpha, Mode mode = Mode::Over) {
    if (p.size() >= 3) {
        Composite(c, FillMask(p), col, alpha, mode);
    }
}

// A stroked polyline (round joins and caps), `width` image px.
inline void Stroke(Canvas& c, const Poly& p, bool closed, float width, Rgb col, float alpha,
                   Mode mode = Mode::Over) {
    if (p.size() >= 2) {
        Composite(c, StrokeMask(p, closed, width), col, alpha, mode);
    }
}

// A dashed polyline (dash and gap lengths in image px).
inline void StrokeDashed(Canvas& c, const Poly& p, float width, float dash, float gap, Rgb col,
                         float alpha) {
    float acc = 0.0f;
    Poly run;
    bool on = true;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        Pt a = p[i];
        const Pt b = p[i + 1];
        float len = std::hypot(b.x - a.x, b.y - a.y);
        while (len > 1e-4f) {
            const float left = (on ? dash : gap) - acc;
            const float step = std::min(left, len);
            const Pt q{a.x + (b.x - a.x) * step / len, a.y + (b.y - a.y) * step / len};
            if (on) {
                if (run.empty()) {
                    run.push_back(a);
                }
                run.push_back(q);
            }
            acc += step;
            len -= step;
            a = q;
            if (acc >= (on ? dash : gap) - 1e-4f) {
                if (on && run.size() >= 2) {
                    Stroke(c, run, false, width, col, alpha);
                }
                run.clear();
                on = !on;
                acc = 0.0f;
            }
        }
    }
    if (on && run.size() >= 2) {
        Stroke(c, run, false, width, col, alpha);
    }
}

// A soft glow around a polygon (additive): the polygon's mask blurred by `sigma` image px (the
// mock's lit trigger).
inline void GlowPoly(Canvas& c, const Poly& p, float sigma, Rgb col, float alpha) {
    if (p.size() >= 3) {
        Composite(c, Blur(FillMask(p), sigma), col, alpha, Mode::Add);
    }
}

// Diagonal hatching (lines of constant x + y, `period` image px apart) clipped to a polygon.
inline void Hatch(Canvas& c, const Poly& clip, float period, float width, Rgb col, float alpha) {
    Mask m = FillMask(clip);
    constexpr float K = 0.70710678f;
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            float& v = m.At(x, y);
            if (v <= 0.0f) {
                continue;
            }
            const float u = (static_cast<float>(x + m.x0 + y + m.y0) + 1.0f) * K;
            const float t = u - period * std::floor(u / period); // 0..period
            v *= Coverage(std::min(t, period - t) - 0.5f * width);
        }
    }
    Composite(c, m, col, alpha);
}

// A small filled arrow head at `tip`, pointing along (dx, dy).
inline void ArrowHead(Canvas& c, Pt tip, float dx, float dy, float size, Rgb col, float alpha) {
    const float l = std::hypot(dx, dy);
    if (l < 1e-6f) {
        return;
    }
    const float ux = dx / l, uy = dy / l;
    const Poly head{{tip.x, tip.y},
                    {tip.x - ux * size - uy * size * 0.55f, tip.y - uy * size + ux * size * 0.55f},
                    {tip.x - ux * size * 0.7f, tip.y - uy * size * 0.7f},
                    {tip.x - ux * size + uy * size * 0.55f, tip.y - uy * size - ux * size * 0.55f}};
    FillPoly(c, head, col, alpha);
}

// A soft glow behind a line of text (additive; the lit callout): the text's coverage blurred.
inline void TextGlow(Canvas& c, const GameText::GameFont* font, const std::string& text, float x,
                     float baseline, Align align, Rgb col, float alpha, float size, float sigma) {
    if (font == nullptr || text.empty()) {
        return;
    }
    GameText::Line line;
    constexpr u32 Pad = 2;
    if (!GameText::RasterizeLine(*font, text.c_str(), size * PageScale, 0.0f, Pad, line)) {
        return;
    }
    const float width = line.advance / PageScale;
    const float pen = align == Align::Left    ? x
                      : align == Align::Right ? x - width
                                              : x - 0.5f * width;
    Mask m;
    m.x0 = static_cast<int>(std::lround(Canvas::ToX(pen))) - static_cast<int>(Pad);
    m.y0 = static_cast<int>(std::lround(Canvas::ToY(baseline) - line.baseline));
    m.w = static_cast<int>(line.width);
    m.h = static_cast<int>(line.height);
    m.a.resize(line.cover.size());
    for (size_t i = 0; i < line.cover.size(); ++i) {
        m.a[i] = line.cover[i] / 255.0f;
    }
    Composite(c, Blur(m, sigma), col, alpha, Mode::Add);
}

} // namespace VR::Page::Draw
