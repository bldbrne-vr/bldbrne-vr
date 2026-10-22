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
#include <mutex>
#include <optional>

#include "core/vr/controls_render.h"
#include "core/vr/game_font.h"
#include "core/vr/vr_page_art.h"
#include "core/vr/vr_page_draw.h"

namespace VR::ControlsView {

using namespace Page::Draw;
using Page::PageScale;

namespace {

// ---- Layout (image px of the page image; the approved mock 3 placed on the page's frame) --------
// The mock's page px map to the page image as x + 53.7, y - 14.7 (both at 4/3 per stage px).

const float CentreX = Canvas::ToX(TitleCentreX); // 786.1: the title's and the key guide's centre
constexpr float ArtW = 240.0f;                    // one controller's box (240 x 470)
constexpr float ArtGap = 12.0f;                   // from the centre line to each controller
constexpr float ArtY = 227.3f;
constexpr float LabelInset = 292.4f;              // the text's edge from the centre line
constexpr float FirstBaseline = 247.3f;           // the first callout
constexpr float LastBaseline = 667.3f;            // the sixth (the mock's pitch: 84)
constexpr float MaxPitch = 140.0f;
constexpr float SubDy = 31.0f;                    // the sub-line under the title
constexpr float TitlePx = 32.0f;                  // 24 stage px
constexpr float SubPx = 26.0f;                    // 19.5 stage px: the smallest text here
constexpr float NoteY = 749.3f;                   // the quiet line above the help rule
constexpr float HelpWidthMax = 1000.0f;           // stage px, then the help wraps to two lines
constexpr float ColumnAvail = 312.0f;             // a callout's text: to the frame's ornament rules

// The Gestures tab: two columns of pictogram + title + sub.
constexpr float PictoSize = 96.0f;
constexpr float GestureTop = 214.0f;
constexpr float GesturePitch = 128.0f;

// Ink (the mock's).
constexpr Rgb Ink{198 / 255.0f, 190 / 255.0f, 172 / 255.0f};
constexpr Rgb Ink2{160 / 255.0f, 152 / 255.0f, 136 / 255.0f};
constexpr Rgb Body{30 / 255.0f, 29 / 255.0f, 27 / 255.0f};
constexpr Rgb Face{46 / 255.0f, 44 / 255.0f, 40 / 255.0f};
constexpr Rgb Dark{22 / 255.0f, 21 / 255.0f, 19 / 255.0f};
constexpr Rgb Cap{58 / 255.0f, 55 / 255.0f, 50 / 255.0f};
constexpr Rgb Button{60 / 255.0f, 58 / 255.0f, 54 / 255.0f};
constexpr Rgb Shade{14 / 255.0f, 13 / 255.0f, 12 / 255.0f};
constexpr Rgb Ring{100 / 255.0f, 96 / 255.0f, 88 / 255.0f};
constexpr Rgb Rim{92 / 255.0f, 88 / 255.0f, 80 / 255.0f};
constexpr Rgb HatchInk{118 / 255.0f, 112 / 255.0f, 98 / 255.0f};
constexpr Rgb LitFill{160 / 255.0f, 120 / 255.0f, 60 / 255.0f};
constexpr Rgb LitEdge{255 / 255.0f, 236 / 255.0f, 196 / 255.0f};
constexpr Rgb LitGlow{255 / 255.0f, 196 / 255.0f, 110 / 255.0f};
constexpr Rgb TouchEdge{232 / 255.0f, 204 / 255.0f, 150 / 255.0f};
constexpr Rgb Lead{128 / 255.0f, 120 / 255.0f, 104 / 255.0f};
constexpr Rgb LeadTouched{190 / 255.0f, 165 / 255.0f, 120 / 255.0f};
constexpr Rgb LeadLit{238 / 255.0f, 196 / 255.0f, 120 / 255.0f};
constexpr Rgb SubInk{162 / 255.0f, 158 / 255.0f, 146 / 255.0f};
constexpr Rgb SubLit{200 / 255.0f, 190 / 255.0f, 162 / 255.0f};
constexpr Rgb TitlePressed{240 / 255.0f, 214 / 255.0f, 160 / 255.0f};
constexpr Rgb NoteInk{140 / 255.0f, 136 / 255.0f, 124 / 255.0f};
constexpr Rgb GlowBand{112 / 255.0f, 92 / 255.0f, 46 / 255.0f};
constexpr Rgb TextGlowInk{255 / 255.0f, 214 / 255.0f, 140 / 255.0f};
constexpr Rgb Letter{206 / 255.0f, 200 / 255.0f, 186 / 255.0f};
constexpr Rgb LetterLit{40 / 255.0f, 28 / 255.0f, 12 / 255.0f};

float StageX(float x) {
    return Canvas::StageX(x);
}
float StageY(float y) {
    return Canvas::StageY(y);
}

// Text in image px (DrawText takes stage px).
float Text(Canvas& c, const GameText::GameFont* font, const std::string& s, float x, float y,
           Align a, Rgb col, float px, float alpha = 1.0f) {
    return DrawText(c, font, s, StageX(x), StageY(y), a, col, alpha, px / PageScale) * PageScale;
}
float TextW(const GameText::GameFont* font, const std::string& s, float px) {
    return TextWidth(font, s, px / PageScale) * PageScale;
}

// The size a line is drawn at to fit `avail` image px: its own, or down to 85% of it (longer
// lines run on; the tests keep every callout's lines short).
float FitPx(const GameText::GameFont* font, const std::string& s, float px, float avail) {
    const float w = TextW(font, s, px);
    return w <= avail || w <= 0.0f ? px : std::max(0.85f * px, px * avail / w);
}

// ---- The controllers in line art ---------------------------------------------------------------

using Lights = std::array<Lit, 7>; // by Part

struct Pen {
    Canvas& c;
    float ox, oy;
    bool mirror; // the left controller: the right one mirrored
    Pt P(float x, float y) const {
        return {ox + (mirror ? ArtW - x : x), oy + y};
    }
    Poly Map(const Poly& p) const {
        return Place(p, ox, oy, 1.0f, mirror ? ArtW : 0.0f);
    }
    Poly E(float cx, float cy, float rx, float ry, float rot = 0.0f, int n = 72) const {
        return Map(Ellipse(cx, cy, rx, ry, rot, n));
    }
};

Rgb FillOf(Lit l, Rgb normal) {
    return l == Lit::Pressed ? LitFill : normal;
}
Rgb EdgeOf(Lit l, Rgb normal) {
    return l == Lit::Pressed ? LitEdge : l == Lit::Touched ? TouchEdge : normal;
}
// A part's glow (pressed: the mock's lit trigger; touched: a faint one).
void PartGlow(Canvas& c, const Poly& p, Lit l, float sigma = 12.0f) {
    if (l == Lit::Pressed) {
        GlowPoly(c, p, sigma, LitGlow, 0.85f);
    } else if (l == Lit::Touched) {
        GlowPoly(c, p, sigma * 0.6f, LitGlow, 0.28f);
    }
}
Lit L(const Lights& lit, Part p) {
    return lit[static_cast<size_t>(p)];
}

// A face button: its shadow, the cap, the letter (the game's font).
void FaceButton(Pen& pen, const GameText::GameFont* font, float bx, float by, float r,
                const std::string& letter, Lit lit) {
    const Pt c = pen.P(bx, by);
    const Poly cap = Ellipse(c.x, c.y - 0.5f, r, r * 0.85f);
    PartGlow(pen.c, cap, lit, 9.0f);
    FillPoly(pen.c, Ellipse(c.x, c.y + 3.0f, r, r * 0.88f), Shade, 1.0f);
    FillPoly(pen.c, cap, FillOf(lit, Button), 1.0f);
    Stroke(pen.c, cap, true, 2.4f, EdgeOf(lit, Ink), 1.0f);
    if (!letter.empty()) {
        Text(pen.c, font, letter, c.x, c.y + 7.5f, Align::Centre,
             lit == Lit::Pressed ? LetterLit : Letter, 21.0f);
    }
}

// Quest Touch (also Touch Plus, HP Reverb G2, PICO, the simple controller): the mock's drawing,
// seen from the front and a little above: the oval top face (the stick on the inner side, two
// buttons on the outer one), the trigger paddle under the face's front, the grip on the
// handle's inner side, the tapered handle bending in toward its end.
void DrawTouch(Pen& pen, const GameText::GameFont* font, const Lights& lit, bool right,
               std::array<std::optional<Pt>, 7>& anchors, bool frame = false) {
    Canvas& c = pen.c;
    constexpr float Rot = 7.0f; // the face's outer side sits a little lower
    const float lw = 3.2f;
    const Poly handle = pen.Map({{80, 112}, {164, 118}, {160, 200}, {150, 300}, {138, 382},
                                 {124, 410}, {104, 412}, {90, 392}, {80, 300}, {74, 200}});
    FillPoly(c, handle, Body, 1.0f);
    Hatch(c,
          pen.Map({{124, 118}, {164, 118}, {160, 200}, {150, 300}, {138, 382}, {124, 410},
                   {116, 300}, {120, 200}}),
          8.0f / 1.41421356f, 1.3f, HatchInk, 150.0f / 255.0f);
    Stroke(c, handle, true, lw, Ink, 1.0f);
    // The grip: a long bump on the handle's inner side.
    const Lit grip = L(lit, Part::Grip);
    const Poly g = pen.Map({{66, 196}, {78, 186}, {84, 300}, {76, 318}, {66, 306}, {62, 250}});
    PartGlow(c, g, grip, 10.0f);
    FillPoly(c, g, grip == Lit::Pressed ? Rgb{150 / 255.0f, 112 / 255.0f, 56 / 255.0f}
                                        : Rgb{42 / 255.0f, 40 / 255.0f, 36 / 255.0f},
             1.0f);
    Stroke(c, g, true, 2.6f, EdgeOf(grip, Ink2), 1.0f);
    // The head: its side wall, then the trigger under it, then the top face.
    const Poly wall = pen.E(120, 100, 104, 54, pen.mirror ? -Rot : Rot);
    FillPoly(c, wall, Dark, 1.0f);
    Stroke(c, wall, true, lw, Ink, 1.0f);
    const Lit trig = L(lit, Part::Trigger);
    const Poly t =
        pen.Map({{94, 134}, {146, 136}, {148, 152}, {138, 172}, {120, 180}, {102, 172}, {92, 152}});
    PartGlow(c, t, trig);
    FillPoly(c, t, FillOf(trig, Rgb{36 / 255.0f, 34 / 255.0f, 31 / 255.0f}), 1.0f);
    Stroke(c, t, true, lw, EdgeOf(trig, Ink), 1.0f);
    Stroke(c, {pen.P(102, 150), pen.P(140, 151)}, false, 1.6f,
           trig == Lit::Pressed ? Rgb{1.0f, 226 / 255.0f, 170 / 255.0f} : Ink2, 200.0f / 255.0f);
    const Poly face = pen.E(120, 86, 104, 54, pen.mirror ? -Rot : Rot);
    FillPoly(c, face, Face, 1.0f);
    Stroke(c, face, true, lw, Ink, 1.0f);
    // A highlight along the face's far rim.
    Poly rim = Arc(119, 84, 93, 48, 200, 320);
    Stroke(c, pen.Map(rim), false, 1.6f, Rim, 1.0f);
    // The thumbstick on the inner side.
    const Lit stick = L(lit, Part::Stick);
    const float sr = pen.mirror ? -Rot : Rot;
    const Poly base = pen.E(74, 86, 34, 27, sr);
    FillPoly(c, base, Dark, 1.0f);
    Stroke(c, base, true, 2.2f, Ink2, 1.0f);
    const Poly cap = pen.E(74, 81, 24, 19, sr);
    PartGlow(c, cap, stick, 9.0f);
    FillPoly(c, cap, FillOf(stick, Cap), 1.0f);
    Stroke(c, cap, true, 2.6f, EdgeOf(stick, Ink), 1.0f);
    for (const float r : {14.0f, 7.0f}) {
        Stroke(c, pen.E(74, 81, r, r * 0.8f, sr), true, 1.2f,
               stick == Lit::Pressed ? Rgb{1.0f, 226 / 255.0f, 170 / 255.0f} : Ring, 1.0f);
    }
    // The face buttons on the outer side: lower (A / X), upper (B / Y).
    if (frame) {
        FaceButton(pen, font, 165, 110, 12.0f, right ? "A" : "v", L(lit, Part::ButtonLow));
        FaceButton(pen, font, 165, 62, 12.0f, right ? "Y" : "^",
                   right ? Lit::Off : L(lit, Part::ButtonHigh));
        FaceButton(pen, font, 141, 86, 12.0f, right ? "X" : ">", Lit::Off);
        FaceButton(pen, font, 189, 86, 12.0f, right ? "B" : "<",
                   right ? L(lit, Part::ButtonHigh) : Lit::Off);
    } else {
        FaceButton(pen, font, 150, 104, 17.0f, right ? "A" : "X", L(lit, Part::ButtonLow));
        FaceButton(pen, font, 178, 70, 17.0f, right ? "B" : "Y", L(lit, Part::ButtonHigh));
    }
    // The menu button (left; the right one is the system button) toward the centre.
    const Lit menu = right ? Lit::Off : L(lit, Part::Menu);
    const Pt m = pen.P(113, 118.5f);
    const Poly mb = Ellipse(m.x, m.y, 9.0f, 6.5f);
    PartGlow(c, mb, menu, 7.0f);
    FillPoly(c, mb, FillOf(menu, Button), 1.0f);
    Stroke(c, mb, true, 2.0f, EdgeOf(menu, Ink2), 1.0f);
    if (!right) {
        for (const float k : {-3.0f, 0.0f, 3.0f}) {
            Stroke(c, {{m.x - 4.0f, m.y + k}, {m.x + 4.0f, m.y + k}}, false, 1.0f,
                   menu == Lit::Pressed ? LetterLit : Rgb{200 / 255.0f, 194 / 255.0f, 180 / 255.0f},
                   1.0f);
        }
    }
    anchors[static_cast<size_t>(Part::Stick)] = pen.P(74, 78);
    anchors[static_cast<size_t>(Part::ButtonLow)] = frame ? pen.P(165, 110) : pen.P(150, 103);
    anchors[static_cast<size_t>(Part::ButtonHigh)] =
        frame ? (right ? pen.P(189, 86) : pen.P(165, 62)) : pen.P(178, 69);
    anchors[static_cast<size_t>(Part::Menu)] = pen.P(113, 118);
    anchors[static_cast<size_t>(Part::Trigger)] = pen.P(120, 160);
    anchors[static_cast<size_t>(Part::Grip)] = pen.P(72, 252);
}

// Valve Index: a tall handle with its strap along the back, a narrow head: the stick at the top,
// the racetrack trackpad under it, A and B beside the pad, the trigger under the head.
void DrawIndex(Pen& pen, const GameText::GameFont* font, const Lights& lit, bool right,
               std::array<std::optional<Pt>, 7>& anchors) {
    Canvas& c = pen.c;
    const float lw = 3.0f;
    // The strap: a loop along the outer side of the handle.
    const Poly strap = pen.Map({{150, 150}, {196, 168}, {206, 260}, {196, 360}, {160, 410},
                                {150, 400}, {178, 352}, {186, 262}, {176, 178}, {146, 166}});
    FillPoly(c, strap, Rgb{24 / 255.0f, 23 / 255.0f, 21 / 255.0f}, 1.0f);
    Hatch(c, strap, 6.0f, 1.1f, HatchInk, 0.55f);
    Stroke(c, strap, true, 2.2f, Ink2, 1.0f);
    // The handle.
    const Poly handle = pen.Map({{86, 120}, {152, 124}, {156, 210}, {150, 320}, {138, 404},
                                 {118, 420}, {100, 410}, {90, 320}, {80, 210}});
    FillPoly(c, handle, Body, 1.0f);
    Hatch(c, pen.Map({{124, 124}, {152, 124}, {156, 210}, {150, 320}, {138, 404}, {120, 416},
                      {116, 320}, {120, 210}}),
          5.66f, 1.3f, HatchInk, 150.0f / 255.0f);
    Stroke(c, handle, true, lw, Ink, 1.0f);
    // The grip: the handle's inner face (force sensing): a long pad.
    const Lit grip = L(lit, Part::Grip);
    const Poly g = pen.Map({{78, 200}, {90, 196}, {96, 330}, {90, 360}, {80, 340}, {74, 260}});
    PartGlow(c, g, grip, 10.0f);
    FillPoly(c, g, grip == Lit::Pressed ? LitFill : Rgb{42 / 255.0f, 40 / 255.0f, 36 / 255.0f},
             1.0f);
    Stroke(c, g, true, 2.4f, EdgeOf(grip, Ink2), 1.0f);
    // The head (a rounded teardrop) and the trigger under it.
    const Lit trig = L(lit, Part::Trigger);
    const Poly t = pen.Map({{98, 136}, {140, 138}, {142, 154}, {132, 176}, {118, 184}, {104, 176},
                            {96, 154}});
    PartGlow(c, t, trig);
    FillPoly(c, t, FillOf(trig, Rgb{36 / 255.0f, 34 / 255.0f, 31 / 255.0f}), 1.0f);
    Stroke(c, t, true, lw, EdgeOf(trig, Ink), 1.0f);
    const Poly wall = pen.Map(RoundRect(56, 30, 184, 142, 52));
    FillPoly(c, wall, Dark, 1.0f);
    const Poly face = pen.Map(RoundRect(56, 24, 184, 134, 52));
    FillPoly(c, face, Face, 1.0f);
    Stroke(c, face, true, lw, Ink, 1.0f);
    // The stick at the top (inner side), the racetrack pad below it.
    const Lit stick = L(lit, Part::Stick);
    const Poly base = pen.E(98, 58, 24, 20);
    FillPoly(c, base, Dark, 1.0f);
    Stroke(c, base, true, 2.0f, Ink2, 1.0f);
    const Poly cap = pen.E(98, 55, 16, 13);
    PartGlow(c, cap, stick, 8.0f);
    FillPoly(c, cap, FillOf(stick, Cap), 1.0f);
    Stroke(c, cap, true, 2.4f, EdgeOf(stick, Ink), 1.0f);
    Stroke(c, pen.E(98, 55, 8, 6.5f), true, 1.2f, Ring, 1.0f);
    const Lit pad = L(lit, Part::Pad);
    const Poly p = pen.Map(RoundRect(84, 80, 114, 126, 15));
    PartGlow(c, p, pad, 8.0f);
    FillPoly(c, p, FillOf(pad, Rgb{40 / 255.0f, 38 / 255.0f, 35 / 255.0f}), 1.0f);
    Stroke(c, p, true, 2.2f, EdgeOf(pad, Ink2), 1.0f);
    // A and B on the outer side, beside the pad.
    FaceButton(pen, font, 150, 108, 13.0f, "A", L(lit, Part::ButtonLow));
    FaceButton(pen, font, 150, 74, 13.0f, "B", L(lit, Part::ButtonHigh));
    (void)right;
    anchors[static_cast<size_t>(Part::Stick)] = pen.P(98, 52);
    anchors[static_cast<size_t>(Part::Pad)] = pen.P(99, 104);
    anchors[static_cast<size_t>(Part::ButtonLow)] = pen.P(150, 107);
    anchors[static_cast<size_t>(Part::ButtonHigh)] = pen.P(150, 73);
    anchors[static_cast<size_t>(Part::Trigger)] = pen.P(120, 164);
    anchors[static_cast<size_t>(Part::Grip)] = pen.P(84, 270);
}

// HTC Vive wand: the sensor ring on top, a long handle with the round trackpad near its top, the
// menu button above the pad, the grip buttons on both sides, the trigger underneath.
void DrawVive(Pen& pen, const GameText::GameFont* font, const Lights& lit, bool right,
              std::array<std::optional<Pt>, 7>& anchors) {
    Canvas& c = pen.c;
    (void)font;
    (void)right;
    const float lw = 3.0f;
    // The ring (a torus seen from above and in front).
    const Poly outer = pen.E(120, 70, 92, 58);
    const Poly inner = pen.E(120, 72, 56, 32);
    FillPoly(c, outer, Body, 1.0f);
    FillPoly(c, inner, Rgb{8 / 255.0f, 8 / 255.0f, 8 / 255.0f}, 0.92f);
    Stroke(c, outer, true, lw, Ink, 1.0f);
    Stroke(c, inner, true, 2.2f, Ink2, 1.0f);
    for (int i = 0; i < 9; ++i) { // the sensor dimples
        const float a = (200.0f + 140.0f * i / 8.0f) * 3.14159265f / 180.0f;
        const Pt d = pen.P(120 + 74 * std::cos(a), 70 + 45 * std::sin(a) + 2);
        FillPoly(c, Ellipse(d.x, d.y, 3.0f, 2.2f), Ink2, 0.8f);
    }
    // The trigger under the ring's front.
    const Lit trig = L(lit, Part::Trigger);
    const Poly t = pen.Map({{100, 150}, {140, 150}, {144, 170}, {132, 196}, {118, 202}, {104, 194},
                            {96, 170}});
    PartGlow(c, t, trig);
    FillPoly(c, t, FillOf(trig, Rgb{36 / 255.0f, 34 / 255.0f, 31 / 255.0f}), 1.0f);
    Stroke(c, t, true, lw, EdgeOf(trig, Ink), 1.0f);
    // The handle.
    const Poly handle = pen.Map({{82, 118}, {158, 118}, {160, 220}, {152, 340}, {142, 420},
                                 {120, 432}, {98, 420}, {88, 340}, {80, 220}});
    FillPoly(c, handle, Body, 1.0f);
    Hatch(c, pen.Map({{126, 118}, {158, 118}, {160, 220}, {152, 340}, {142, 420}, {124, 430},
                      {120, 340}, {122, 220}}),
          5.66f, 1.3f, HatchInk, 150.0f / 255.0f);
    Stroke(c, handle, true, lw, Ink, 1.0f);
    // The grip buttons on both sides of the handle.
    const Lit grip = L(lit, Part::Grip);
    for (const bool side : {false, true}) {
        const Poly g = side ? pen.Map({{154, 250}, {164, 246}, {166, 300}, {156, 306}})
                            : pen.Map({{86, 250}, {76, 246}, {74, 300}, {84, 306}});
        PartGlow(c, g, grip, 8.0f);
        FillPoly(c, g, grip == Lit::Pressed ? LitFill : Rgb{42 / 255.0f, 40 / 255.0f, 36 / 255.0f},
                 1.0f);
        Stroke(c, g, true, 2.2f, EdgeOf(grip, Ink2), 1.0f);
    }
    // The menu button and the round trackpad on the handle's top.
    const Lit menu = L(lit, Part::Menu);
    const Poly mb = pen.Map(RoundRect(110, 132, 130, 144, 6));
    PartGlow(c, mb, menu, 7.0f);
    FillPoly(c, mb, FillOf(menu, Button), 1.0f);
    Stroke(c, mb, true, 2.0f, EdgeOf(menu, Ink2), 1.0f);
    const Lit pad = L(lit, Part::Pad);
    const Poly p = pen.E(120, 186, 30, 26);
    PartGlow(c, p, pad, 9.0f);
    FillPoly(c, p, FillOf(pad, Rgb{40 / 255.0f, 38 / 255.0f, 35 / 255.0f}), 1.0f);
    Stroke(c, p, true, 2.4f, EdgeOf(pad, Ink), 1.0f);
    Stroke(c, pen.E(120, 186, 14, 12), true, 1.2f, Ring, 0.9f);
    FillPoly(c, pen.E(120, 228, 5, 4), Button, 1.0f); // the system button
    Stroke(c, pen.E(120, 228, 5, 4), true, 1.4f, Ink2, 1.0f);
    anchors[static_cast<size_t>(Part::Menu)] = pen.P(120, 138);
    anchors[static_cast<size_t>(Part::Pad)] = pen.P(120, 186);
    anchors[static_cast<size_t>(Part::Trigger)] = pen.P(120, 180);
    anchors[static_cast<size_t>(Part::Grip)] = pen.P(80, 276);
}

// Windows Mixed Reality: the LED ring round the top, the stick and the trackpad side by side on
// the top face, the menu button, the trigger, the grip button.
void DrawWmr(Pen& pen, const GameText::GameFont* font, const Lights& lit, bool right,
             std::array<std::optional<Pt>, 7>& anchors) {
    Canvas& c = pen.c;
    (void)font;
    (void)right;
    const float lw = 3.0f;
    // The ring, standing behind the top face.
    const Poly outer = pen.E(132, 64, 96, 56, -8);
    const Poly inner = pen.E(132, 66, 74, 38, -8);
    FillPoly(c, outer, Body, 1.0f);
    FillPoly(c, inner, Rgb{8 / 255.0f, 8 / 255.0f, 8 / 255.0f}, 0.92f);
    Stroke(c, outer, true, lw, Ink, 1.0f);
    Stroke(c, inner, true, 2.0f, Ink2, 1.0f);
    // The handle.
    const Poly handle = pen.Map({{84, 132}, {156, 136}, {158, 220}, {150, 330}, {138, 410},
                                 {118, 422}, {100, 410}, {90, 330}, {80, 220}});
    FillPoly(c, handle, Body, 1.0f);
    Hatch(c, pen.Map({{124, 136}, {156, 136}, {158, 220}, {150, 330}, {138, 410}, {122, 420},
                      {116, 330}, {120, 220}}),
          5.66f, 1.3f, HatchInk, 150.0f / 255.0f);
    Stroke(c, handle, true, lw, Ink, 1.0f);
    const Lit grip = L(lit, Part::Grip);
    const Poly g = pen.Map({{72, 210}, {84, 204}, {88, 300}, {80, 312}, {70, 300}});
    PartGlow(c, g, grip, 9.0f);
    FillPoly(c, g, grip == Lit::Pressed ? LitFill : Rgb{42 / 255.0f, 40 / 255.0f, 36 / 255.0f},
             1.0f);
    Stroke(c, g, true, 2.4f, EdgeOf(grip, Ink2), 1.0f);
    const Lit trig = L(lit, Part::Trigger);
    const Poly t = pen.Map({{98, 150}, {142, 152}, {144, 168}, {134, 190}, {118, 196}, {104, 190},
                            {96, 168}});
    PartGlow(c, t, trig);
    FillPoly(c, t, FillOf(trig, Rgb{36 / 255.0f, 34 / 255.0f, 31 / 255.0f}), 1.0f);
    Stroke(c, t, true, lw, EdgeOf(trig, Ink), 1.0f);
    // The top face.
    const Poly face = pen.Map(RoundRect(56, 92, 184, 150, 26));
    FillPoly(c, face, Face, 1.0f);
    Stroke(c, face, true, lw, Ink, 1.0f);
    const Lit stick = L(lit, Part::Stick);
    const Poly base = pen.E(90, 120, 20, 16);
    FillPoly(c, base, Dark, 1.0f);
    Stroke(c, base, true, 2.0f, Ink2, 1.0f);
    const Poly cap = pen.E(90, 117, 13, 11);
    PartGlow(c, cap, stick, 8.0f);
    FillPoly(c, cap, FillOf(stick, Cap), 1.0f);
    Stroke(c, cap, true, 2.4f, EdgeOf(stick, Ink), 1.0f);
    const Lit pad = L(lit, Part::Pad);
    const Poly p = pen.E(146, 121, 25, 20);
    PartGlow(c, p, pad, 9.0f);
    FillPoly(c, p, FillOf(pad, Rgb{40 / 255.0f, 38 / 255.0f, 35 / 255.0f}), 1.0f);
    Stroke(c, p, true, 2.2f, EdgeOf(pad, Ink2), 1.0f);
    const Lit menu = L(lit, Part::Menu);
    const Poly mb = pen.Map(RoundRect(108, 154, 128, 164, 5));
    PartGlow(c, mb, menu, 7.0f);
    FillPoly(c, mb, FillOf(menu, Button), 1.0f);
    Stroke(c, mb, true, 1.8f, EdgeOf(menu, Ink2), 1.0f);
    anchors[static_cast<size_t>(Part::Stick)] = pen.P(90, 114);
    anchors[static_cast<size_t>(Part::Pad)] = pen.P(146, 121);
    anchors[static_cast<size_t>(Part::Menu)] = pen.P(118, 159);
    anchors[static_cast<size_t>(Part::Trigger)] = pen.P(120, 178);
    anchors[static_cast<size_t>(Part::Grip)] = pen.P(78, 258);
}

void DrawController(Canvas& c, const GameText::GameFont* font, Family f, bool right, float ox,
                    float oy, const Lights& lit, std::array<std::optional<Pt>, 7>& anchors) {
    Pen pen{c, ox, oy, !right};
    switch (f) {
    case Family::Frame:
        DrawTouch(pen, font, lit, right, anchors, true);
        return;
    case Family::Index:
        DrawIndex(pen, font, lit, right, anchors);
        return;
    case Family::Vive:
        DrawVive(pen, font, lit, right, anchors);
        return;
    case Family::Wmr:
        DrawWmr(pen, font, lit, right, anchors);
        return;
    case Family::Touch:
    case Family::Simple:
        break;
    }
    DrawTouch(pen, font, lit, right, anchors);
}

// ---- The pictograms (the Gestures tab), in a PictoSize box at (x, y) ----------------------------

void DrawPicto(Canvas& c, Picto p, float x, float y, Lit lit, const GameText::GameFont* font,
               const std::string& a_name, const std::string& b_name) {
    const Rgb ink = lit == Lit::Pressed ? LitEdge : lit == Lit::Touched ? TouchEdge : Ink;
    const Rgb ink2 = lit == Lit::Pressed ? LeadLit : Ink2;
    const float w = 3.0f;
    const auto P = [x, y](float u, float v) { return Pt{x + u, y + v}; };
    if (lit == Lit::Pressed) {
        GlowPoly(c, Place(RoundRect(10, 10, 86, 86, 30), x, y), 14.0f, LitGlow, 0.35f);
    }
    switch (p) {
    case Picto::Swing: {
        // A blade sweeping through an arc, the arrow at its end.
        const Poly arc = Place(Arc(48, 56, 38, 30, 200, 340), x, y);
        Stroke(c, arc, false, w, ink, 1.0f);
        const Pt end = arc.back();
        const Pt before = arc[arc.size() - 3];
        ArrowHead(c, end, end.x - before.x, end.y - before.y, 12.0f, ink, 1.0f);
        Stroke(c, {P(24, 70), P(58, 18)}, false, 4.0f, ink, 1.0f);     // the blade
        Stroke(c, {P(18, 62), P(32, 76)}, false, 3.0f, ink2, 1.0f);    // the guard
        Stroke(c, {P(24, 70), P(16, 82)}, false, 4.0f, ink2, 1.0f);    // the hilt
        break;
    }
    case Picto::Gun: {
        const Poly gun = Place({{8, 40}, {50, 40}, {50, 50}, {30, 50}, {26, 66}, {16, 66}, {18, 50},
                                {8, 50}},
                               x, y);
        FillPoly(c, gun, lit == Lit::Pressed ? LitFill : Body, 1.0f);
        Stroke(c, gun, true, 2.4f, ink, 1.0f);
        StrokeDashed(c, {P(56, 45), P(88, 45)}, 2.2f, 6.0f, 4.0f, ink2, 1.0f);
        for (int i = 0; i < 6; ++i) { // the burst at the far end
            const float a = i * 60.0f * 3.14159265f / 180.0f;
            Stroke(c, {P(88 + 3 * std::cos(a), 45 + 3 * std::sin(a)),
                       P(88 + 8 * std::cos(a), 45 + 8 * std::sin(a))},
                   false, 1.8f, ink, 1.0f);
        }
        break;
    }
    case Picto::Vial: {
        // The vial (a syringe of blood) stabbing down into a wrist.
        const Poly body = Place({{30, 14}, {44, 22}, {28, 52}, {14, 44}}, x, y);
        FillPoly(c, body, Rgb{92 / 255.0f, 18 / 255.0f, 16 / 255.0f}, 1.0f);
        Stroke(c, body, true, 2.4f, ink, 1.0f);
        Stroke(c, {P(37, 18), P(44, 6)}, false, 3.0f, ink2, 1.0f);  // the plunger
        Stroke(c, {P(21, 48), P(14, 62)}, false, 2.0f, ink, 1.0f);  // the needle
        Stroke(c, {P(10, 70), P(86, 70)}, false, w, ink2, 1.0f);    // the forearm
        Stroke(c, {P(10, 84), P(86, 84)}, false, w, ink2, 1.0f);
        Stroke(c, {P(62, 58), P(70, 52)}, false, 2.0f, ink, 0.8f);  // the strike marks
        Stroke(c, {P(66, 64), P(78, 60)}, false, 2.0f, ink, 0.8f);
        break;
    }
    case Picto::Arc: {
        // Five cards fanned on an arc above a hand.
        for (int i = 0; i < 5; ++i) {
            const float a = (-150.0f + 30.0f * i) * 3.14159265f / 180.0f;
            const float cx = 48 + 34 * std::cos(a), cy = 62 + 34 * std::sin(a);
            const Poly r = Place(RoundRect(-7, -9, 7, 9, 2), x + cx, y + cy);
            FillPoly(c, r, i == 2 && lit == Lit::Pressed ? LitFill : Face, 1.0f);
            Stroke(c, r, true, 2.0f, i == 2 ? ink : ink2, 1.0f);
        }
        Stroke(c, Place(RoundRect(38, 66, 58, 90, 8), x, y), true, w, ink, 1.0f); // the hand
        Stroke(c, {P(48, 62), P(48, 44)}, false, 2.0f, ink2, 1.0f);
        ArrowHead(c, P(48, 40), 0.0f, -1.0f, 8.0f, ink2, 1.0f);
        break;
    }
    case Picto::Throw: {
        // A flask thrown along a dashed arc to a burst.
        Poly path;
        for (int i = 0; i <= 24; ++i) {
            const float t = i / 24.0f;
            path.push_back(P(18 + 66 * t, 64 - 190 * t + 196 * t * t)); // up to y 18, down to 70
        }
        StrokeDashed(c, path, 2.2f, 6.0f, 4.0f, ink2, 1.0f);
        const Poly flask = Place(Ellipse(18, 64, 9, 11), x, y);
        FillPoly(c, flask, lit == Lit::Pressed ? LitFill : Face, 1.0f);
        Stroke(c, flask, true, 2.4f, ink, 1.0f);
        Stroke(c, {P(18, 52), P(18, 46)}, false, 3.0f, ink, 1.0f);
        for (int i = 0; i < 7; ++i) {
            const float a = (-180.0f + i * 30.0f) * 3.14159265f / 180.0f;
            Stroke(c, {P(84 + 4 * std::cos(a), 78 + 4 * std::sin(a)),
                       P(84 + 11 * std::cos(a), 78 + 11 * std::sin(a))},
                   false, 2.0f, ink, 1.0f);
        }
        break;
    }
    case Picto::Belt: {
        // A belt with its buckle, a vial on it, the hand reaching down to it.
        Stroke(c, {P(6, 56), P(90, 60)}, false, 8.0f, ink2, 0.9f);
        Stroke(c, Place(RoundRect(40, 48, 56, 68, 3), x, y), true, 2.4f, ink, 1.0f);
        const Poly vial = Place(RoundRect(66, 54, 78, 84, 5), x, y);
        FillPoly(c, vial, Rgb{92 / 255.0f, 18 / 255.0f, 16 / 255.0f}, 1.0f);
        Stroke(c, vial, true, 2.2f, ink, 1.0f);
        Stroke(c, Place(RoundRect(62, 10, 82, 34, 8), x, y), true, w, ink, 1.0f); // the hand
        Stroke(c, {P(72, 36), P(72, 46)}, false, 2.0f, ink2, 1.0f);
        ArrowHead(c, P(72, 50), 0.0f, 1.0f, 7.0f, ink2, 1.0f);
        break;
    }
    case Picto::Evade: {
        // Head and shoulders, a ghost of them leaning aside, and a duck arrow.
        StrokeDashed(c, Place(Ellipse(30, 34, 12, 12, 0, 32), x, y), 2.0f, 4.0f, 3.0f, ink2, 1.0f);
        Stroke(c, Place(Ellipse(56, 34, 12, 12), x, y), true, w, ink, 1.0f);
        Stroke(c, Place(Arc(56, 78, 30, 22, 200, 340), x, y), false, w, ink, 1.0f);
        Stroke(c, {P(40, 20), P(24, 20)}, false, 2.0f, ink2, 1.0f);
        ArrowHead(c, P(20, 20), -1.0f, 0.0f, 7.0f, ink2, 1.0f);
        Stroke(c, {P(84, 22), P(84, 50)}, false, 2.0f, ink2, 1.0f);
        ArrowHead(c, P(84, 54), 0.0f, 1.0f, 7.0f, ink2, 1.0f);
        break;
    }
    case Picto::GripAdjust: {
        // The two buttons held, and the blade turning in the hand.
        for (int i = 0; i < 2; ++i) {
            const Poly b = Place(Ellipse(22 + 26 * i, 26, 11, 10), x, y);
            FillPoly(c, b, lit == Lit::Pressed ? LitFill : Button, 1.0f);
            Stroke(c, b, true, 2.2f, ink, 1.0f);
            const std::string& n = i == 0 ? a_name : b_name;
            if (n.size() <= 2) {
                Text(c, font, n, x + 22 + 26 * i, y + 26 + 7.0f, Align::Centre,
                     lit == Lit::Pressed ? LetterLit : Letter, 18.0f);
            }
        }
        Stroke(c, {P(30, 82), P(80, 50)}, false, 4.0f, ink, 1.0f);
        Stroke(c, Place(Arc(56, 70, 22, 22, -40, 60), x, y), false, 2.0f, ink2, 1.0f);
        ArrowHead(c, P(56 + 22 * 0.5f, 70 + 22 * 0.866f), -0.866f, 0.5f, 7.0f, ink2, 1.0f);
        break;
    }
    case Picto::Pointer: {
        Stroke(c, Place(RoundRect(8, 58, 30, 88, 8), x, y), true, w, ink, 1.0f);
        StrokeDashed(c, {P(26, 60), P(70, 30)}, 2.0f, 5.0f, 4.0f, ink2, 1.0f);
        Stroke(c, Place(Ellipse(76, 26, 10, 10), x, y), true, 2.0f, Gilt, 1.0f);
        break;
    }
    case Picto::None:
        break;
    }
}

// ---- Text helpers ------------------------------------------------------------------------------

// The help line in one or two lines (wrapped at the space nearest the middle).
std::vector<std::string> WrapHelp(const GameText::GameFont* font, const std::string& help) {
    if (TextWidth(font, help) <= HelpWidthMax) {
        return {help};
    }
    size_t best = std::string::npos;
    float best_d = 1e9f;
    for (size_t i = 1; i + 1 < help.size(); ++i) {
        if (help[i] != ' ') {
            continue;
        }
        const float w1 = TextWidth(font, help.substr(0, i));
        const float w2 = TextWidth(font, help.substr(i + 1));
        const float d = std::max(w1, w2);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best == std::string::npos) {
        return {help};
    }
    return {help.substr(0, best), help.substr(best + 1)};
}

// The selection glow behind a callout (the mock's warm band): a vertical Gaussian round the
// title's middle, ramped in along x over the text.
void Band(Canvas& g, float x0, float x1, float cy, float strength) {
    constexpr float Sigma = 22.0f;
    constexpr float Ramp = 60.0f;
    const int Y0 = std::max(0, static_cast<int>(cy - 3 * Sigma)),
              Y1 = std::min(g.h, static_cast<int>(cy + 3 * Sigma));
    const int X0 = std::max(0, static_cast<int>(x0 - Ramp)),
              X1 = std::min(g.w, static_cast<int>(x1 + Ramp));
    for (int y = Y0; y < Y1; ++y) {
        const float gy = std::exp(-0.5f * ((y + 0.5f - cy) / Sigma) * ((y + 0.5f - cy) / Sigma));
        for (int x = X0; x < X1; ++x) {
            float v = std::clamp(std::min((x + 0.5f - x0) / Ramp + 0.5f, (x1 - x - 0.5f) / Ramp + 0.5f),
                                 0.0f, 1.0f);
            v = v * v * (3.0f - 2.0f * v);
            g.Add(x, y, GlowBand.r, GlowBand.g, GlowBand.b, gy * v * strength);
        }
    }
}

// Where each callout sits (image px): its text anchor and baseline.
struct Slot {
    float x;        // the title's edge (right edge on the left column, left edge on the right)
    float baseline;
    bool right;
};

std::vector<Slot> Slots(const View& v) {
    std::vector<Slot> out(v.callouts.size());
    const bool gestures = v.tab == static_cast<int>(Tab::Gestures);
    for (const bool right : {false, true}) {
        std::vector<size_t> col;
        for (size_t i = 0; i < v.callouts.size(); ++i) {
            if (v.callouts[i].right == right) {
                col.push_back(i);
            }
        }
        const size_t n = col.size();
        for (size_t k = 0; k < n; ++k) {
            Slot s;
            s.right = right;
            if (gestures) {
                const float px = right ? CentreX + 40.0f : CentreX - 520.0f;
                s.x = px + PictoSize + 20.0f;
                s.baseline = GestureTop + GesturePitch * static_cast<float>(k) + 44.0f;
            } else {
                const float pitch =
                    n > 1 ? std::min(MaxPitch, (LastBaseline - FirstBaseline) / static_cast<float>(n - 1))
                          : 0.0f;
                // Fewer callouts than six: spread over the controller, centred on it.
                const float span = pitch * static_cast<float>(n > 0 ? n - 1 : 0);
                const float top = FirstBaseline + 0.5f * ((LastBaseline - FirstBaseline) - span);
                s.x = right ? CentreX + LabelInset : CentreX - LabelInset;
                s.baseline = top + pitch * static_cast<float>(k);
            }
            out[col[k]] = s;
        }
    }
    return out;
}

// The text box of a callout (image px), for the pointer and the halo.
struct Box {
    float x0, y0, x1, y1;
};
Box CalloutBox(const View& v, const Slot& s, const Callout& c, const GameText::GameFont* font) {
    const float w = std::max(TextW(font, c.title, TitlePx), TextW(font, c.sub, SubPx));
    const bool gestures = v.tab == static_cast<int>(Tab::Gestures);
    if (gestures) {
        const float px = s.x - PictoSize - 20.0f;
        return {px - 6.0f, s.baseline - 50.0f, s.x + std::max(w, 200.0f) + 8.0f,
                s.baseline - 50.0f + PictoSize + 12.0f};
    }
    const float y0 = s.baseline - 30.0f, y1 = s.baseline + (c.sub.empty() ? 10.0f : SubDy + 9.0f);
    return s.right ? Box{s.x - 10.0f, y0, s.x + w + 10.0f, y1}
                   : Box{s.x - w - 10.0f, y0, s.x + 10.0f, y1};
}

// The page's frame without its rows: the game's Environment page with the row rules, the column
// divider and the rows' grain left out (the Brightness page's open middle).
bool InControlsFrame(const PageArt::Piece& p) {
    return !(p.y0 >= 250.0f && p.y1 <= 625.0f && p.y1 > 271.0f);
}

} // Anonymous namespace

View MakeView(const Model& model, const Page::Glyphs& glyphs) {
    View v;
    for (const char* t : TabNames) {
        v.tabs.emplace_back(t);
    }
    v.tab = static_cast<int>(model.GetTab());
    v.family = model.GetFamily();
    v.parts = model.Parts();
    v.callouts = model.Callouts();
    v.lights = model.Lights();
    v.focus = model.Focus();
    v.hover = model.Hover();
    v.help = model.Help();
    v.note = Note(model.GetTab(), model.GetConfig(), model.GetFamily());
    v.glyphs = glyphs;
    v.keys = {{Page::KeyGlyph::DpadUD, "Select"},
              {Page::KeyGlyph::PrevNext, "Page"},
              {Page::KeyGlyph::Back, "Return"}};
    return v;
}

std::vector<HitBox> HitBoxes(const View& v, const GameText::GameFont* font) {
    std::vector<HitBox> boxes;
    const TabLayout t = LayoutTabs(v.tabs, v.glyphs, font);
    for (size_t i = 0; i < v.tabs.size(); ++i) {
        boxes.push_back({HitBox::Kind::Tab, static_cast<int>(i), t.x[i] - 8.0f,
                         TitleBaseline - 26.0f, t.x[i] + t.width[i] + 8.0f, TitleBaseline + 10.0f});
    }
    boxes.push_back({HitBox::Kind::Prev, -1, t.prev_cx - 0.5f * t.prev_w - 4.0f, t.badge_cy - 18.0f,
                     t.prev_cx + 0.5f * t.prev_w + 4.0f, t.badge_cy + 18.0f});
    boxes.push_back({HitBox::Kind::Next, -1, t.next_cx - 0.5f * t.next_w - 4.0f, t.badge_cy - 18.0f,
                     t.next_cx + 0.5f * t.next_w + 4.0f, t.badge_cy + 18.0f});
    const auto slots = Slots(v);
    for (size_t i = 0; i < v.callouts.size(); ++i) {
        const Box b = CalloutBox(v, slots[i], v.callouts[i], font);
        boxes.push_back({HitBox::Kind::Callout, static_cast<int>(i), StageX(b.x0), StageY(b.y0),
                         StageX(b.x1), StageY(b.y1)});
    }
    return boxes;
}

const HitBox* HitTest(const std::vector<HitBox>& boxes, float x, float y) {
    for (const HitBox& b : boxes) {
        if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1) {
            return &b;
        }
    }
    return nullptr;
}

bool Render(const Page::Assets& assets, const View& v, Page::Image& page_out,
            Page::Image& glow_out) {
    if (!assets.Ready()) {
        return false;
    }
    const GameText::GameFont* font = assets.font;
    Canvas page;
    Canvas glow;

    // 1. The frame: the settings page's, its middle open (cached per atlas).
    {
        static std::mutex cache_mutex;
        static const u8* cache_atlas = nullptr;
        static std::vector<float> cache;
        std::scoped_lock lk{cache_mutex};
        if (cache_atlas != assets.common3.rgba || cache.size() != page.px.size()) {
            // The page's panel exactly (vr_page_render.cpp): its middle opaque in VR, its soft
            // edges the game's (PanelDim; VR_PAGE_SEE_THROUGH=1: the game's dim everywhere).
            for (int i = 0; i < PageArt::FrameBackdropCount; ++i) {
                DrawPiece(page, assets, PageArt::Frame[i], 0.0f, 0.0f, false,
                          PageArt::ScreenDimAlpha, 1e9f, true, PanelOpaque());
            }
            for (const PageArt::Piece& p : PageArt::Frame) {
                if (InControlsFrame(p)) {
                    DrawPiece(page, assets, p, 0.0f, 0.0f, false, 1.0f);
                }
            }
            cache = page.px;
            cache_atlas = assets.common3.rgba;
        } else {
            page.px = cache;
        }
    }

    // 2. The tabs (Gameplay / Menus / Gestures) in the title line.
    DrawTabStrip(page, font, v.tabs, v.tab, v.glyphs);

    const auto slots = Slots(v);
    const auto lit_of = [&](size_t i) { return i < v.lights.size() ? v.lights[i] : Lit::Off; };
    const bool gestures = v.tab == static_cast<int>(Tab::Gestures);

    // 3. The controllers (Gameplay, Menus) and the leader lines to their parts.
    std::array<std::array<std::optional<Pt>, 7>, 2> anchors{};
    if (!gestures) {
        for (const bool right : {false, true}) {
            Lights lights{};
            for (size_t i = 0; i < v.callouts.size(); ++i) {
                const Callout& c = v.callouts[i];
                if (c.part < 0 || c.part >= static_cast<int>(v.parts.size())) {
                    continue;
                }
                const DrawnPart& d = v.parts[static_cast<size_t>(c.part)];
                if (d.right == right) {
                    Lit& l = lights[static_cast<size_t>(d.part)];
                    l = std::max(l, lit_of(i));
                }
            }
            const float ox = right ? CentreX + ArtGap : CentreX - ArtGap - ArtW;
            DrawController(page, font, v.family, right, ox, ArtY, lights,
                           anchors[right ? 1 : 0]);
        }
    }

    // 4. The callouts.
    for (size_t i = 0; i < v.callouts.size(); ++i) {
        const Callout& c = v.callouts[i];
        const Slot& s = slots[i];
        const Lit lit = lit_of(i);
        const bool focused = static_cast<int>(i) == v.focus;
        // The swing (no button) in the group headers' teal, as in the approved mock.
        const Rgb title_ink = focused                 ? LabelSelected
                              : lit == Lit::Pressed   ? TitlePressed
                              : (c.part < 0 && !gestures) ? HeaderTeal
                                                      : LabelGrey;
        const Rgb sub_ink = focused || lit == Lit::Pressed ? SubLit : SubInk;
        if (gestures) {
            const float px = s.x - PictoSize - 20.0f;
            const float py = s.baseline - 44.0f;
            // The grip adjust's pictogram names its two buttons (wherever the layout put them).
            const std::string a = c.controls.size() > 0 ? NameOf(v.parts, c.controls[0]) : "";
            const std::string b = c.controls.size() > 1 ? NameOf(v.parts, c.controls[1]) : "";
            DrawPicto(page, c.picto, px, py, lit, font, a, b);
            Text(page, font, c.title, s.x, s.baseline, Align::Left, title_ink, TitlePx);
            Text(page, font, c.sub, s.x, s.baseline + SubDy, Align::Left, sub_ink, SubPx);
            if (focused) {
                const float w = TextW(font, c.title, TitlePx);
                Band(glow, s.x - 20.0f, s.x + w + 20.0f, s.baseline - 11.0f, 0.85f);
                TextGlow(glow, font, c.title, StageX(s.x), StageY(s.baseline), Align::Left,
                         TextGlowInk, 0.55f, TitlePx / PageScale, 7.0f);
            }
            continue;
        }
        const Align align = s.right ? Align::Left : Align::Right;
        const float w = Text(page, font, c.title, s.x, s.baseline, align, title_ink,
                             FitPx(font, c.title, TitlePx, ColumnAvail));
        Text(page, font, c.sub, s.x, s.baseline + SubDy, align, sub_ink,
             FitPx(font, c.sub, SubPx, ColumnAvail));
        if (focused) {
            const float sw = std::max(w, TextW(font, c.sub, SubPx));
            if (s.right) {
                Band(glow, s.x - 20.0f, s.x + sw + 30.0f, s.baseline - 11.0f, 0.85f);
            } else {
                Band(glow, s.x - sw - 30.0f, s.x + 20.0f, s.baseline - 11.0f, 0.85f);
            }
            TextGlow(glow, font, c.title, StageX(s.x), StageY(s.baseline), align, TextGlowInk,
                     0.55f, TitlePx / PageScale, 7.0f);
        }
        // The leader line: out from the text, then to the part (a gesture: its own mark).
        const Rgb lead = lit == Lit::Pressed ? LeadLit : lit == Lit::Touched ? LeadTouched : Lead;
        const float lead_a = lit == Lit::Off ? 235.0f / 255.0f : 1.0f;
        const float dir = s.right ? -1.0f : 1.0f;
        const Pt from{s.x + dir * 12.0f, s.baseline - 10.0f};
        const Pt knee{s.x + dir * 30.0f, s.baseline - 10.0f};
        std::optional<Pt> to;
        if (c.part >= 0 && c.part < static_cast<int>(v.parts.size())) {
            const DrawnPart& d = v.parts[static_cast<size_t>(c.part)];
            to = anchors[d.right ? 1 : 0][static_cast<size_t>(d.part)];
        }
        if (to) {
            Stroke(page, {from, knee, *to}, false, 2.0f, lead, lead_a);
            FillPoly(page, Ellipse(to->x, to->y, 3.5f, 3.5f, 0.0f, 16), lead, lead_a);
        } else if (c.picto == Picto::Swing) {
            // The swing: a short arc beside the right controller instead of a line (the mock's).
            const float cx = CentreX + ArtGap + 120.0f + 95.0f, cy = s.baseline - 52.0f;
            const Rgb ink = lit == Lit::Pressed ? LeadLit : HeaderTeal;
            const Poly arc = Arc(cx, cy, 55.0f, 35.0f, 20.0f, 150.0f);
            if (lit == Lit::Pressed) {
                GlowPoly(page, Place(RoundRect(-56, -36, 56, 36, 30), cx, cy), 10.0f, LitGlow, 0.25f);
            }
            Stroke(page, arc, false, 2.0f, ink, 210.0f / 255.0f);
            const Pt end = arc.back();
            const Pt before = arc[arc.size() - 3];
            ArrowHead(page, end, end.x - before.x, end.y - before.y, 11.0f, ink, 210.0f / 255.0f);
        } else if (c.picto == Picto::Pointer) {
            // The pointer: a dashed ray from the right controller's head toward its words.
            const Pt a{CentreX + ArtGap + 160.0f, ArtY + 60.0f};
            StrokeDashed(page, {a, knee}, 2.0f, 6.0f, 5.0f, Lead, lead_a);
        }
    }

    // 5. The pointer's halo round the callout (or tab) it is on.
    if (v.hover >= 0 && v.hover < static_cast<int>(v.callouts.size())) {
        const Box b = CalloutBox(v, slots[static_cast<size_t>(v.hover)],
                                 v.callouts[static_cast<size_t>(v.hover)], font);
        Halo(page, StageX(b.x0), StageY(b.y0), StageX(b.x1), StageY(b.y1), false);
    } else if (v.hover_tab != -1) {
        for (const HitBox& b : HitBoxes(v, font)) {
            const bool match = (b.kind == HitBox::Kind::Tab && b.index == v.hover_tab) ||
                               (b.kind == HitBox::Kind::Prev && v.hover_tab == -2) ||
                               (b.kind == HitBox::Kind::Next && v.hover_tab == -3);
            if (match) {
                const bool round = b.kind != HitBox::Kind::Tab;
                if (round) {
                    const float cx = 0.5f * (b.x0 + b.x1), cy = 0.5f * (b.y0 + b.y1);
                    const float rr = 15.0f / PageScale;
                    Halo(page, cx - rr * 1.6f, cy - rr, cx + rr * 1.6f, cy + rr, false);
                } else {
                    Halo(page, b.x0, b.y0 + 4.0f, b.x1, b.y1 - 2.0f, false);
                }
                break;
            }
        }
    }

    // 6. The quiet line, the help (one or two lines) and the key guide.
    Text(page, font, v.note, CentreX, NoteY, Align::Centre, NoteInk, SubPx);
    const auto lines = WrapHelp(font, v.help);
    if (lines.size() == 1) {
        DrawText(page, font, lines[0], HelpCentreX, HelpBaseline, Align::Centre, HelpKhaki);
    } else {
        DrawText(page, font, lines[0], HelpCentreX, HelpBaseline - 26.5f, Align::Centre, HelpKhaki);
        DrawText(page, font, lines[1], HelpCentreX, HelpBaseline, Align::Centre, HelpKhaki);
    }
    DrawKeyGuide(page, assets, v.keys, v.glyphs);

    page.ToImage(page_out);
    glow.ToImage(glow_out);
    return true;
}

} // namespace VR::ControlsView
