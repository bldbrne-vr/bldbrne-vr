// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "common/types.h"

// Where the game's UI goes on the UI layer (renderer_vulkan/vr_ui_layer.h, the presenter's
// compositor layers). Pure: no emulator state, unit tested in tests/test_ui_layout.cpp.
//
// The game draws its UI on a 1920x1080 "stage" (Scaleform), scaled to the 16:9 band of the
// render. The layer cuts it up:
//  - Full-screen menus: the whole band on a curved canvas fixed in space where you looked.
//  - Small dialogs and notes: a crop around the box, on the same canvas, its text up to
//    ui_dialog_em_deg tall (body text is 24 stage px) but the box at most ui_dialog_max_deg wide
//    (DialogDegPerPx: the game's boxes are ~1000 stage px wide, so the cap sets their size).
//  - Full-screen menus with ui_menu_fit: a crop around their content (StepMenuCrop), as large as
//    fits the view (MenuDegPerPx): a mostly empty list (the headstone's travel list, the Options
//    grid) is shown larger, never a corner out of view.
//  - Every canvas crop stays inside the view both eyes share (CanvasDegPerPx: its corners within
//    ui_menu_corner_yaw_deg / ui_menu_corner_pitch_deg of its centre).
//  - Gameplay HUD ("belt"): the HP / stamina / items cluster, the echoes / insight cluster and
//    the lower group (action prompts, item pickups, subtitles, the boss bar), each a segment
//    fixed to the view (HudAnchor::View: the clusters in its lower corners, the lower group
//    centred, as the flat game has them) or of a band low in the view that follows the head
//    lazily (HudAnchor::Band, StepHudFollow). The lower group is "dynamic": only its wide,
//    horizontally connected elements (HudSplit) go to the band; a lock-on cursor, an enemy's HP
//    bar or a damage number that happens to be drawn there stays where the game draws it.
//  - Everything else (lock-on cursor, enemy tags, YOU DIED and other banners): the "rest", where
//    the game draws it, on a quad at the frame's own head pose. A gameplay frame whose UI covers
//    the screen's edges (the death screen, a loading page) goes onto it whole (StepScreenWide).
namespace VR::Ui {

// The game's UI stage.
constexpr float StageWidth = 1920.0f;
constexpr float StageHeight = 1080.0f;
constexpr float BodyTextPx = 24.0f; // stage px of the menus' body text
constexpr float DegToRadF = 0.0174532925f;

struct StageRect {
    float x0{}, y0{}, x1{}, y1{}; // stage px, end exclusive
    [[nodiscard]] float Width() const {
        return x1 - x0;
    }
    [[nodiscard]] float Height() const {
        return y1 - y0;
    }
};

// One HUD band segment: a stage rectangle shown at a place on the band.
struct Segment {
    StageRect rect{};
    float yaw_deg{};       // the segment's centre right (+) of the band anchor
    float row_pitch_deg{}; // the segment's centre below (-) the band anchor
    float deg_per_px{};    // angular size of one stage px
    const char* name{""};
    // Shown while a small dialog or note is open too (the status clusters; the lower group would
    // overlap the dialog's crop).
    bool with_dialogs{false};
    // Content-classified (HudSplit): only wide elements go to the band, the rest of the rectangle
    // stays on the rest quad. Fixed segments take their whole rectangle.
    bool dynamic{false};
    [[nodiscard]] float WidthDeg() const {
        return rect.Width() * deg_per_px;
    }
    [[nodiscard]] float HeightDeg() const {
        return rect.Height() * deg_per_px;
    }
    // The text size (BodyTextPx) on it.
    [[nodiscard]] float EmDeg() const {
        return BodyTextPx * deg_per_px;
    }
};

enum class HudLayout : s32 {
    Belt = 0, // the segments below on the band
    Game = 1, // no band: the whole UI on the rest quad (where the game draws it)
};

// What the belt's segments are fixed to (vr.ini ui_hud_anchor).
enum class HudAnchor : s32 {
    // The view (the head): the status clusters stay in its lower-left and lower-right corners and
    // the lower group in its lower middle, wherever you look, as on the flat screen.
    View = 0,
    // A band that follows the head lazily (StepHudFollow): reading it never moves it, but turning
    // the head carries the clusters across the view.
    Band = 1,
};

constexpr u32 MaxSegments = 3;

struct HudSegments {
    std::array<Segment, MaxSegments> segment{};
    u32 count{};
};

// The band's design scale: its angles below are designed for it and scale with vr.ini
// ui_hud_scale_deg, so a larger HUD keeps its proportions and never overlaps itself.
constexpr float BeltDefaultDegPerPx = 0.035f;
// The lower group (prompts, pickups, subtitles, boss bar) on the band is read in passing: 8/7
// larger (body text 0.96 deg at the design scale).
constexpr float LowerGroupScale = 8.0f / 7.0f;

// The view anchor (HudAnchor::View). The flat game's HUD content edges (stage px): the HP bar's
// left end and the echoes counter's right end, put ViewCornerYawDeg (vr.ini
// ui_hud_corner_yaw_deg) left and right of the view's centre; the status clusters' rows (their
// rect's middle, stage y 135) ViewCornerPitchDeg (ui_hud_corner_pitch_deg) below it; the lower
// group's middle (stage y 887) ViewLowerPitchDeg below it. At the default ui_hud_scale_deg
// (0.042: body text 1.0 deg) the HP / items cluster spans -30..-15 deg (its item name; a long HP
// bar reaches toward the middle as on the flat screen), the echoes cluster +19..+30, both from
// -18 to -24.5 deg; an action prompt -9..-12.5, subtitles and a boss bar down to about -15.
constexpr float HudLeftContentX = 106.0f;
constexpr float HudRightContentX = 1808.0f;
constexpr float ViewCornerYawDeg = 30.0f;
constexpr float ViewCornerPitchDeg = -21.0f;
constexpr float ViewLowerPitchDeg = -10.0f;

// The belt (Bloodborne's HUD, measured in stage px: HP / stamina / items x 106..377 y 63..219,
// echoes / insight x 1549..1808 y 68..160, an action prompt x 491..1416 y 868..946):
//  - status_left x 60..960 (room for an HP bar of ~2300 HP), its left edge 21 deg left;
//  - status_right x 1520..1840, its left edge 10.5 deg right (never under a long HP bar);
//  - lower x 300..1620 y 700..1075, yaw-centred (dynamic; split up to x 1920, CutRect).
// Rows: the lower group's centre 10 deg and the status clusters' 21.6 deg below the anchor (+
// pitch_offset): an action prompt at about -12 deg, HP at -19..-24 deg, all within 21 deg of the
// anchor's yaw.
// HudAnchor::View: the same rects, every one at deg_per_px, the clusters' outer content edges
// corner_yaw_deg left and right of the view's centre and their rows corner_pitch_deg (+
// pitch_offset) below it, the lower group centred ViewLowerPitchDeg (+ pitch_offset) below it.
inline HudSegments ComputeHudLayout(HudLayout layout, float deg_per_px, float pitch_offset_deg,
                                    HudAnchor anchor = HudAnchor::Band,
                                    float corner_yaw_deg = ViewCornerYawDeg,
                                    float corner_pitch_deg = ViewCornerPitchDeg) {
    HudSegments out{};
    if (layout != HudLayout::Belt) {
        return out;
    }
    const float s = std::clamp(deg_per_px, 0.01f, 0.1f);
    const float f = s / BeltDefaultDegPerPx; // every angle scales with the HUD
    const StageRect left{60.0f, 20.0f, 960.0f, 250.0f};
    const StageRect right{1520.0f, 20.0f, 1840.0f, 250.0f};
    const StageRect lower{300.0f, 700.0f, 1620.0f, 1075.0f};
    if (anchor == HudAnchor::View) {
        const float corner = std::clamp(corner_yaw_deg, 5.0f, 60.0f);
        const float rows = std::clamp(corner_pitch_deg, -45.0f, 45.0f) + pitch_offset_deg;
        out.segment[0] = {left,
                          -corner + (0.5f * (left.x0 + left.x1) - HudLeftContentX) * s,
                          rows,
                          s,
                          "status_left",
                          true,
                          false};
        out.segment[1] = {right,
                          corner - (HudRightContentX - 0.5f * (right.x0 + right.x1)) * s,
                          rows,
                          s,
                          "status_right",
                          true,
                          false};
        out.segment[2] = {lower, 0.0f, ViewLowerPitchDeg + pitch_offset_deg, s, "lower", false,
                          true};
        out.count = 3;
        return out;
    }
    constexpr float LeftEdgeDeg = -21.0f;
    constexpr float RightLeftEdgeDeg = 10.5f;
    out.segment[0] = {left,
                      (LeftEdgeDeg + 0.5f * left.Width() * BeltDefaultDegPerPx) * f,
                      -21.6f * f + pitch_offset_deg,
                      s,
                      "status_left",
                      true,
                      false};
    out.segment[1] = {right,
                      (RightLeftEdgeDeg + 0.5f * right.Width() * BeltDefaultDegPerPx) * f,
                      -21.6f * f + pitch_offset_deg,
                      s,
                      "status_right",
                      true,
                      false};
    out.segment[2] = {lower, 0.0f, -10.0f * f + pitch_offset_deg, s * LowerGroupScale, "lower",
                      false, true};
    out.count = 3;
    return out;
}

// A raised boss bar (bloodborne_enemy_bars.cpp, ui_boss_bar=1) sits in the status clusters'
// rows (stage y ~160): while it is up, the HP cluster's cut out of the rest quad stops here (the
// cluster's content ends by x ~377), so the whole bar stays on the rest quad, centred as the game
// lays it out, the same in both eyes (2026-10-18: cut at x 960, its left part went with the HP
// cluster and the rest started at the view's middle). The HP piece is unchanged (it samples the
// whole band); a longer HP bar's end shows where the game draws it meanwhile.
constexpr float StatusLeftBossCutX = 400.0f;

// Dialogue must never be split between the lower HUD and the world-aligned rest
// quad: they have different sizes/depths. Include both margins and the final five
// stage rows, otherwise the subtitle backing leaves a detached bottom strip.
inline void FitDialogue(HudSegments& hud, bool dialogue) {
    if (!dialogue || hud.count < 3) return;
    auto& lower = hud.segment[2];
    lower.rect = {0.0f, 700.0f, StageWidth, StageHeight};
    lower.row_pitch_deg -= 1.5f;
}

// ---- Area names ----------------------------------------------------------------------------
// Entering an area the game shows its name for ~5 s in the stage's lower-right corner, fading in
// and out: the words right-aligned at x ~1706 over a rule x ~1125..1794 and an ink smear, y
// ~840..1005 (measured in a capture of the Hunter's Dream, 2026-10-12). Where the flat game has
// it, it sat 20..35 deg right of the view's centre, and the lower group's rect cut it at x 1620
// (the rest of it on the rest quad, at another size and depth). So the lower group is split up to
// the stage's right edge (CutRect), and while a name is up the lower piece shows that corner with
// the words' middle (AreaNameCenterX) straight ahead, AreaNameDropDeg below the lower group's row,
// AreaNameDistanceM away (easier on the eyes than the HUD's 2 m). A lower-group HUD element
// reaching past AreaNameRightX is an area name: the action prompts end at 1416, the item pickups
// and the boss bar are centred (subtitles are not counted: dialogue).
// Its dark backing (the ink smear, a dim down to the stage's last row) is keyed out on the GPU
// (vr_ui_resolve.comp KeyAreaName, inside AreaNameKeyX0 / AreaNameKeyY0): the words and the rule
// stay. Its last rows were below the lower group's rect (y 1075) and showed on the rest quad as a
// detached dark strip, so while a name is up the cut and the piece reach the stage's bottom.
constexpr float AreaNameRightX = 1650.0f;
// ... and short of the edge: a fade or a loading page spans the stage (its rows reach ~1920).
constexpr float AreaNameMaxRightX = 1880.0f;
// The words' middle (x 1420..1706, right-aligned at 1706); the rule's (x 1125..1794) is 1460: it
// put the words ~4 deg right of the view's centre.
constexpr float AreaNameCenterX = 1563.0f;
constexpr float AreaNameDropDeg = 2.0f; // below the lower group's row (VR_TUNE ui.area_drop_deg)
constexpr float AreaNameKeyX0 = 900.0f; // the corner keyed and kept on the piece whole (stage px;
constexpr float AreaNameKeyY0 = 780.0f; // vr_ui_resolve.comp has the same numbers)
// Its backing is keyed from further left (vr_ui_resolve.comp InAreaKey): the piece shows from ~x
// 753, and the smear's tail between it and AreaNameKeyX0 stayed a dark block (2026-10-13).
constexpr float AreaNameKeyLeftX = 600.0f;
constexpr float AreaNameDistanceM = 3.0f;
constexpr float AreaNameHoldS = 0.5f;  // its fade-out's faint tail, a missed measurement
constexpr float AreaNameReachDeg = 34.0f; // the piece's left edge (the view both eyes share)

// The stage rect the GPU splits for a segment: a dynamic one up to the stage's right edge, and
// while an area name is up (`area_name`) to its bottom too.
inline StageRect CutRect(const Segment& seg, bool area_name = false) {
    StageRect r = seg.rect;
    if (seg.dynamic) {
        r.x1 = StageWidth;
        if (area_name) {
            r.y1 = StageHeight;
        }
    }
    return r;
}

// An area name keeps its own place over the screen-wide picture: arriving after a warp, its ink
// smear holds the screen-wide measure on (40% of the border, the 2026-10-13 run) for the ~6 s it
// is up, and the whole frame then went onto the rest quad, the name 20..35 deg right.
constexpr bool AreaNameOverScreenWide(float hud_right_px, bool dialogue, bool dead) {
    return !dialogue && !dead && hud_right_px > AreaNameRightX && hud_right_px < AreaNameMaxRightX;
}

struct AreaNameState {
    bool on{false};
    float clear_s{0.0f}; // time since one was last seen
};

// One update: `hud_right_px` the right end of the lower group's HUD elements in the newest
// measured frame (stage px, 0 = none), `dialogue` (subtitles up), `dt` (s since the last update).
// Returns whether one is up.
constexpr bool StepAreaName(AreaNameState& st, float hud_right_px, bool dialogue, float dt) {
    if (!dialogue && hud_right_px > AreaNameRightX) {
        st = {true, 0.0f};
        return true;
    }
    if (st.on) {
        st.clear_s += std::max(dt, 0.0f);
        if (st.clear_s >= AreaNameHoldS) {
            st = {};
        }
    }
    return st.on;
}

// The lower piece while an area name is up: up to the stage's right and bottom edges (CutRect),
// the name's words (`centre_x`) straight ahead, its rows `drop_deg` lower (its size unchanged),
// its left edge AreaNameReachDeg away (a prompt shown with it keeps its middle).
inline void CentreAreaName(HudSegments& hud, float centre_x = AreaNameCenterX,
                           float drop_deg = AreaNameDropDeg) {
    if (hud.count < 3) {
        return;
    }
    auto& lower = hud.segment[2];
    const float y1 = lower.rect.y1;
    lower.rect.x0 = std::max(lower.rect.x0, centre_x - AreaNameReachDeg / lower.deg_per_px);
    lower.rect.x1 = StageWidth;
    lower.rect.y1 = StageHeight;
    lower.yaw_deg = (0.5f * (lower.rect.x0 + lower.rect.x1) - centre_x) * lower.deg_per_px;
    // The rect's middle moved down with its bottom: its rows stay put, then drop.
    lower.row_pitch_deg -= 0.5f * (StageHeight - y1) * lower.deg_per_px + drop_deg;
}

// ---- The HUD fade (vr.ini ui_hand_fade) -----------------------------------------------------
// A HUD piece is a far layer drawn over everything nearer, so it fades while something of yours
// is in front of its content, as seen from the head (the presenter's UiPieceOccluded): `level` 1
// only the blade, 2 a hand, an arm, the gun or the hunter's torso, 3 one of the open quick-item
// arc's cards or the left hand holding a quick item or the vial (0: nothing); `parts` (levels 1
// and 2): which of the blade, a gun and your own hand / arm / body are in front, each checked on
// its own (HudInFront*). What it fades to:
//  - level 3: 10% (what you read or aim with there: a far layer over it would hide it);
//  - the HP and echoes clusters: level 2 15%, level 1 `blade_alpha` (ui_hud_blade_alpha, 45%);
//  - the lower group (`lower`: the action prompts, item pickups, subtitles and the boss bar) while
//    it holds something to read (`reading`, VR::IsHudTextUp: an action prompt, a text window, an
//    NPC line, an item pop-up, or not known): `blade_alpha` for a weapon, the blade (not while an
//    NPC talks, `talking`: their subtitles) or a gun, and never below `lower_alpha` for your own
//    hands, arms and body (ui_hud_lower_alpha, default 1: they do not fade it at all); the lowest
//    of those. Before 2026-10-09 an action prompt also faded to 15% behind your hands looking down
//    at an item (feedback: keep the blade's 45%, drop the hands' and the body's 15%).
//  - the lower group with nothing to read (the boss bar alone): as the clusters, and whole behind
//    the blade alone while an NPC talks, as it always was.
// 1: the piece does not fade.
constexpr float HudFadeLevel3Alpha = 0.1f;
constexpr float HudFadeLevel2Alpha = 0.15f;
constexpr u32 HudInFrontBlade = 1u; // the blade of the weapon in the right hand
constexpr u32 HudInFrontGun = 2u;   // a gun the game reports in the left hand (along its barrel)
constexpr u32 HudInFrontOwn = 4u;   // your own hand, wrist, forearm, upper arm or torso

constexpr float HudFadeFloor(u32 level, u32 parts, bool lower, bool talking, bool reading,
                             float blade_alpha, float lower_alpha) {
    if (level == 0) {
        return 1.0f;
    }
    if (level >= 3) {
        return HudFadeLevel3Alpha;
    }
    const float base = level == 2 ? HudFadeLevel2Alpha : blade_alpha;
    if (!lower) {
        return base;
    }
    if (!reading) {
        return level == 1 && talking ? 1.0f : base;
    }
    float floor = 1.0f;
    if ((parts & HudInFrontBlade) != 0 && !talking) {
        floor = std::min(floor, blade_alpha);
    }
    if ((parts & HudInFrontGun) != 0) {
        floor = std::min(floor, blade_alpha);
    }
    if ((parts & HudInFrontOwn) != 0) {
        floor = std::min(floor, std::clamp(std::max(HudFadeLevel2Alpha, lower_alpha), 0.0f, 1.0f));
    }
    return floor;
}

// ---- The echoes / insight counters (vr.ini ui_counters) ------------------------------------
// The belt's right cluster (status_right: the echoes and insight counters) in gameplay:
//  - LookDown (the default): hidden; shown while the head is pitched at least show_deg below the
//    horizon (the headset's own pitch: physically looking down), hidden again once it comes back
//    above show_deg - CountersHysteresisDeg. A value that changes never shows them.
//  - Always: as the flat game. Hidden: never in gameplay.
// A small dialog or note shows them in every mode, as a full menu's HUD does (ui_menu_hud).
enum class CountersMode : s32 {
    LookDown = 0,
    Always = 1,
    Hidden = 2,
};
constexpr u32 CountersSegment = 1; // status_right in ComputeHudLayout's segments (both anchors)
constexpr float CountersHysteresisDeg = 6.0f;

// One step of the look-down switch: `shown` before, the head's pitch (degrees, + = up).
constexpr bool StepCountersLookDown(bool shown, float head_pitch_deg, float show_deg) {
    const float down = -head_pitch_deg;
    return shown ? down > show_deg - CountersHysteresisDeg : down >= show_deg;
}

// Whether the counters are shown: `dialog` a small dialog or note is open, `looking_down` the
// look-down switch.
constexpr bool CountersWanted(CountersMode mode, bool dialog, bool looking_down) {
    return dialog || mode == CountersMode::Always ||
           (mode == CountersMode::LookDown && looking_down);
}

// Whether the counters sit raised (vr.ini ui_counters_raise_deg above their corner): while the
// look-down switch shows them, and while they fade out after it (`raised` before); a dialog that
// shows them without looking down puts them back in the corner.
constexpr bool CountersRaised(CountersMode mode, bool looking_down, bool wanted, bool raised) {
    if (mode != CountersMode::LookDown) {
        return false;
    }
    if (looking_down) {
        return true;
    }
    return wanted ? false : raised;
}

// ---- The look-down tray (vr.ini ui_counters_pos, ui_status_left) ------------------------------
// Where the counters go when you look down (CountersRaised says they are placed by the look-down
// switch): LowerMiddle (the default) centred below the view's centre, larger, together with the
// HP / stamina / items cluster (status_left) when that too is shown by looking down: you look down
// and they appear down there, one tidy group (2026-10-05). Corner: their HUD corner;
// Raised: ui_counters_raise_deg above it (the HUD-feel lane's placement).
enum class CountersPos : s32 {
    LowerMiddle = 0,
    Corner = 1,
    Raised = 2,
};

// The HP / stamina / vials / bullets / quick-item cluster (status_left) in gameplay. With the
// Blood Gauge on the wrist (hud_vitals=wrist) it is hidden like the counters and shown with them
// by looking down (vr.ini ui_status_left=auto: LookDown; classic: Always).
enum class StatusLeftMode : s32 {
    LookDown = 0,
    Always = 1,
    Hidden = 2,
};
constexpr u32 StatusLeftSegment = 0; // in ComputeHudLayout's segments (both anchors)

constexpr StatusLeftMode StatusLeftOf(s32 setting, bool wrist) {
    if (setting < 0) {
        return wrist ? StatusLeftMode::LookDown : StatusLeftMode::Always;
    }
    return static_cast<StatusLeftMode>(std::clamp<s32>(setting, 0, 2));
}

// Whether status_left is shown: as the counters (CountersWanted), a small dialog or note always.
constexpr bool StatusLeftWanted(StatusLeftMode mode, bool dialog, bool looking_down) {
    return dialog || mode == StatusLeftMode::Always ||
           (mode == StatusLeftMode::LookDown && looking_down);
}

// The look-down mode of status_left as a CountersMode (CountersRaised: placed by looking down).
constexpr CountersMode StatusLeftAsCounters(StatusLeftMode mode) {
    return mode == StatusLeftMode::LookDown ? CountersMode::LookDown
           : mode == StatusLeftMode::Hidden ? CountersMode::Hidden
                                            : CountersMode::Always;
}

// The clusters' content in stage px: the HP bar's left end (HudLeftContentX), the cluster's right
// end before it is measured (the kit save, 712 HP: the quick item's name "Molotov Cocktail" ends at
// x 468, the bars near 377; a longer HP bar or item name reaches further, measured in gameplay
// frames, TrayLatch), the echoes counter's left end and right end (HudRightContentX).
constexpr float HudLeftContentRightDefaultX = 468.0f;
constexpr float CountersContentLeftX = 1549.0f;
constexpr float HudContentTopY = 63.0f;     // the HP bar's top, stage px
constexpr float HudContentBottomY = 219.0f; // the quick item's box bottom, stage px

// The counters' content rows (stage y): the echoes counter's top and the insight counter's bottom.
constexpr float CountersContentTopY = 68.0f;
constexpr float CountersContentBottomY = 160.0f;

struct TrayConfig {
    // The tray's top (its content's: the HP bar's top, stage y 63, or the counters' alone) this
    // many degrees below (-) the view's centre.
    float top_deg{-14.0f};
    float scale{1.3f};   // x the HUD's deg per px (body text 1.3 deg at the defaults)
    float gap_deg{1.5f}; // between the two clusters
    // true: one narrow column, the HP cluster over the counters, each centred (clear of the hands
    // at the sides); false: side by side, HP cluster | gap | counters, centred as a group.
    bool stacked{true};
};

// Places the tray (HudAnchor::View segments): `left` status_left goes there (its content's right
// end `left_right_x`, stage px), `counters` the counters do; their deg per px x scale, centred on
// the view's centre below its centre (TrayConfig). A segment that does not go there keeps its
// place (status_left shown always stays in its corner).
inline void PlaceTray(HudSegments& hud, bool left, bool counters, float left_right_x,
                      const TrayConfig& c) {
    if (hud.count <= CountersSegment || (!left && !counters)) {
        return;
    }
    Segment& l = hud.segment[StatusLeftSegment];
    Segment& r = hud.segment[CountersSegment];
    const float s = r.deg_per_px * std::clamp(c.scale, 0.5f, 2.5f);
    const float right_x = std::clamp(left_right_x, HudLeftContentX + 100.0f, l.rect.x1);
    const float lw = left ? (right_x - HudLeftContentX) * s : 0.0f;
    const float rw = counters ? (HudRightContentX - CountersContentLeftX) * s : 0.0f;
    const float gap = left && counters ? std::max(c.gap_deg, 0.0f) : 0.0f;
    // A segment's yaw / row for its content's left end at `x0` and top at `top` (degrees).
    const auto place = [s](Segment& seg, float content_x, float content_top_y, float x0,
                           float top) {
        seg.deg_per_px = s;
        seg.yaw_deg = x0 + (0.5f * (seg.rect.x0 + seg.rect.x1) - content_x) * s;
        seg.row_pitch_deg = top - (0.5f * (seg.rect.y0 + seg.rect.y1) - content_top_y) * s;
    };
    if (c.stacked) {
        float top = c.top_deg;
        if (left) {
            place(l, HudLeftContentX, HudContentTopY, -0.5f * lw, top);
            top -= (HudContentBottomY - HudContentTopY) * s + gap;
        }
        if (counters) {
            place(r, CountersContentLeftX, CountersContentTopY, -0.5f * rw, top);
        }
        return;
    }
    float x = -0.5f * (lw + gap + rw); // the group's left end (degrees, + = right)
    if (left) {
        place(l, HudLeftContentX, HudContentTopY, x, c.top_deg);
        x += lw + gap;
    }
    if (counters) {
        // The same row as the HP cluster (their tops 5 stage px apart, as on the flat screen).
        place(r, CountersContentLeftX, HudContentTopY, x, c.top_deg);
    }
}

// The HP cluster's right end used by the tray: follows the measured content (stage px, <= 0 none)
// while the cluster is not in the tray, only grows while it is (nothing moves while you read it).
struct TrayLatch {
    float right_x{HudLeftContentRightDefaultX};
};

inline float StepTrayLatch(TrayLatch& st, bool in_tray, float measured_right_x,
                           float max_x = 960.0f) {
    if (measured_right_x > HudLeftContentX) {
        const float m = std::min(measured_right_x, max_x);
        if (!in_tray || m > st.right_x + 0.5f) {
            st.right_x = in_tray ? std::max(st.right_x, m) : m;
        }
    }
    return st.right_x;
}

// How a dynamic segment tells the game's HUD from world-anchored UI (stage px): per row, the
// horizontal runs of UI (gaps up to gap_px bridged) at least min_width_px wide are HUD; a row
// within dilate_px of such a row takes its extent too (text ascenders, a boss's name above its
// bar), widened by margin_px. A lock-on cursor (~50 px), an enemy's HP bar or a damage number is
// narrower and stays on the rest quad unless it touches a HUD element.
struct HudSplit {
    float min_width_px{320.0f};
    float gap_px{40.0f};
    float dilate_px{40.0f};
    float margin_px{24.0f};
};

// A small dialog's content box is measured inside this stage rect (the status clusters' fixed
// rects are left out too): the lock-on cursor or a subtitle at the screen's edges never holds it
// open.
constexpr StageRect DialogBoxRegion{240.0f, 0.0f, 1680.0f, 1080.0f};

// The rest quad's distance with the belt (vr.ini ui_rest_depth_m=0): lock-on cursors and enemy
// tags are drawn at their target, and at melee range (2-5 m) a quad at 3 m puts them within about
// 0.3 deg of the target's own disparity in either eye.
constexpr float RestDepthDefaultM = 3.0f;

// A gameplay frame whose UI reaches the screen's edges is one picture, not a HUD: the death
// screen's dark backdrop with YOU DIED over it, the loading page after it. Cut into the belt, the
// parts inside the status clusters' rects would show in the view's lower corners (the top of
// "YOU" and a dark patch in the HP corner, the rest of the words without it on the rest quad).
// Such frames go whole onto the rest quad (lined up with the eyes as before) and the belt's
// pieces are left out. The measure is the part of the band's 8 px border holding UI (a uniform
// fade is the veil, already taken out of the UI): on from ScreenWideOn, off once it stayed below
// ScreenWideOff for ScreenWideOffFrames measured frames, so a backdrop fading out, or the death
// screen handing over to the loading page (5-17% of the border for 5-8 frames in the mock), does
// not flash the HUD pieces in between.
constexpr float ScreenWideOn = 0.6f;
constexpr float ScreenWideOff = 0.15f;
constexpr u32 ScreenWideOffFrames = 12;
constexpr u32 BorderEdgePx = 8;

// The band pixels within BorderEdgePx of its border (the resolve's border count).
constexpr u64 BorderPixels(u32 width, u32 height) {
    const u64 w = width, h = height;
    return w <= 2 * BorderEdgePx || h <= 2 * BorderEdgePx
               ? w * h
               : w * h - (w - 2 * BorderEdgePx) * (h - 2 * BorderEdgePx);
}

struct ScreenWideState {
    bool on{false};
    u32 low{0}; // measured frames in a row below ScreenWideOff while on
    u64 seq{0}; // the last frame stepped (a measurement is taken once)
};

// One measured gameplay frame (`seq`: its frame number; a frame already taken is skipped).
// Returns whether `on` changed.
constexpr bool StepScreenWide(ScreenWideState& st, u64 seq, u64 border_ui_px, u32 band_width,
                              u32 band_height) {
    if (seq == st.seq) {
        return false;
    }
    st.seq = seq;
    const u64 total = BorderPixels(band_width, band_height);
    const float share =
        total == 0 ? 0.0f : static_cast<float>(border_ui_px) / static_cast<float>(total);
    const bool was = st.on;
    if (!st.on) {
        st.on = share >= ScreenWideOn;
        st.low = 0;
    } else if (share >= ScreenWideOff) {
        st.low = 0;
    } else if (++st.low >= ScreenWideOffFrames) {
        st.on = false;
        st.low = 0;
    }
    return st.on != was;
}

// Large gameplay messages use one world-anchored title, below the horizon by default.
// The native UI replay omits their rectangular dim/panel draws; original glyphs and glow
// are retained. Do not stretch the rest of the UI band behind the title.
// YOU DIED occupies x 612..1304, y 205..304 on the game's 1920x1080 stage.
constexpr StageRect DeathTitleRect{540.0f, 170.0f, 1380.0f, 340.0f};
constexpr float DeathTextWidthPx = 692.0f; // YOU DIED's width on the stage
constexpr float DeathDimTop = 420.0f;      // the backdrop rows below the title (stage y)

struct DeathTitleSize {
    float width_m{};
    float height_m{};
};

// The title quad's size: the words `text_deg` across at `distance_m`.
inline DeathTitleSize DeathTitleQuad(float distance_m, float text_deg) {
    const float half = 0.5f * std::clamp(text_deg, 5.0f, 120.0f) * DegToRadF;
    const float m_px = 2.0f * distance_m * std::tan(half) / DeathTextWidthPx;
    return {DeathTitleRect.Width() * m_px, DeathTitleRect.Height() * m_px};
}

// Other messages share a wider crop and a common letter scale. Measured stage bounds:
// PREY SLAUGHTERED x 252..1667, BLOOD ECHOES RETRIEVED x 385..1536, y 214..312.
// Start below the normal HUD item-name baseline near y 198.
// A new message retains the existing anchor and restarts the minimum lifetime.
// fe.gfx has clips only for these SetTextEffect slots. Keep event detection and
// native backdrop removal on the same list (including victory/multiplayer results).
constexpr bool BannerTextEffect(u32 type) {
    return type == 1 || type == 2 || type == 4 || type == 5 || type == 7 || type == 15 ||
           type == 16 || type == 17;
}

constexpr StageRect BannerTitleRect{200.0f, 202.0f, 1720.0f, 330.0f};
constexpr float BannerTextWidthPx = 1415.0f;
constexpr float BannerHoldS = 6.0f;

// A text effect whose type was not seen (queued and played between two samples, type 0) is a
// banner only with a banner's screen-wide dim within UnknownBannerNs: an item pickup and an item use
// queue one too (2026-10-22: 50-degree titles of whatever the HUD showed in the banner's rows).
// While the hunter is dead such a one is still the death screen (the caller never waits then).
constexpr s64 UnknownBannerNs = 1'000'000'000;
struct UnknownBannerGate {
    bool pending{false};
    s64 started_ns{0};

    // `now_ns` is the steady clock, not the clamped UI animation delta: flat frames
    // and loading must not pause this deadline. Expire before accepting a late dim.
    // `dropped` reports an expired pending event once. A new event opens a new window.
    constexpr bool Step(bool unknown, bool screen_wide, s64 now_ns, bool& dropped) {
        dropped = false;
        if (unknown) {
            pending = true;
            started_ns = now_ns;
        }
        if (!pending) {
            return false;
        }
        if (now_ns < started_ns || now_ns - started_ns >= UnknownBannerNs) {
            *this = {};
            dropped = true;
            return false;
        }
        if (screen_wide) {
            *this = {};
            return true;
        }
        return false;
    }
};

// The title's cut-out: YOU DIED's (`death`), or the banner's of text-effect `type` (1 PREY
// SLAUGHTERED, 4 BLOOD ECHOES RETRIEVED; any other, BannerTitleRect).
constexpr StageRect TitleRect(bool death, u32 type = 0) {
    return death       ? DeathTitleRect
           : type == 1 ? StageRect{212.0f, BannerTitleRect.y0, 1708.0f, BannerTitleRect.y1}
           : type == 4 ? StageRect{345.0f, BannerTitleRect.y0, 1575.0f, BannerTitleRect.y1}
                       : BannerTitleRect;
}

// The title quad's size: YOU DIED `death_text_deg` across, or a banner's cut-out at the scale of
// BannerTextWidthPx `banner_text_deg` across, at `distance_m`.
inline DeathTitleSize TitleQuad(bool death, u32 type, float distance_m, float death_text_deg,
                                float banner_text_deg) {
    if (death) {
        return DeathTitleQuad(distance_m, death_text_deg);
    }
    const float half = 0.5f * std::clamp(banner_text_deg, 5.0f, 120.0f) * DegToRadF;
    const float m_px = 2.0f * distance_m * std::tan(half) / BannerTextWidthPx;
    const StageRect r = TitleRect(false, type);
    return {r.Width() * m_px, r.Height() * m_px};
}

struct DeathTitleState {
    bool on{false};
    bool screen{false}; // within the death screen's screen-wide period: lasts until it ends
    float left_s{0.0f}; // minimum lifetime after the game starts a banner
    float clear_s{0.0f}; // tolerate short gaps in the asynchronous border measurement
};

// One update: `enabled` (ui_death_distance_m > 0), `screen_wide` (the shown frame is a
// screen-wide gameplay frame), `dead` (the hunter's HP is 0), `head` (a head pose to place it),
// `banner` (the game just played a banner, ui_banner_title), `dt` (s since the last update).
// The title starts with a banner, or with a screen-wide period while dead; it lasts until that
// period has stayed clear for 0.35 s, and at least BannerHoldS after the last banner.
// Returns true when it starts (place it where the head looks now).
constexpr bool StepDeathTitle(DeathTitleState& st, bool enabled, bool screen_wide, bool dead,
                              bool head, bool banner = false, float dt = 0.0f) {
    if (!enabled) {
        st = {};
        return false;
    }
    bool start = false;
    if (banner && head) {
        start = !st.on;
        st.on = true;
        st.left_s = BannerHoldS;
    } else if (!st.on && screen_wide && dead && head) {
        start = true;
        st.on = true;
        st.left_s = 0.0f;
    }
    if (!st.on) {
        return false;
    }
    dt = std::max(dt, 0.0f);
    st.left_s = std::max(0.0f, st.left_s - dt);
    if (screen_wide) {
        st.screen = true;
        st.clear_s = 0.0f;
        return start;
    }
    st.clear_s += dt;
    // The border is a delayed GPU measurement, not the banner's lifetime. Its
    // fade can briefly uncover the border while the words are still visible.
    // Never put those words back on the high gameplay HUD during that gap.
    if (st.left_s <= 0.0f && (!st.screen || st.clear_s >= 0.35f) && !start) {
        st = {};
        return false;
    }
    return start;
}

// The canvas crop of a small dialog (an OK or YES/NO box: the box is ~985 stage px wide) or a
// note (its window spans x 437..1487), centred: the fallback when the box was never measured.
inline StageRect DialogCrop(bool note) {
    const float w = note ? 1160.0f : 1120.0f;
    const float h = note ? 680.0f : 480.0f;
    return {(StageWidth - w) * 0.5f, (StageHeight - h) * 0.5f, (StageWidth + w) * 0.5f,
            (StageHeight + h) * 0.5f};
}

// Stage px -> band px of a `band_width` wide band (the band is the stage scaled).
inline float BandScale(float band_width) {
    return band_width / StageWidth;
}

// The canvas's horizontal size in degrees: a full menu `menu_width_deg`; a crop so its body
// text is `em_deg` tall.
inline float CanvasWidthDeg(const StageRect& rect, bool crop, float menu_width_deg, float em_deg) {
    return crop ? rect.Width() * em_deg / BodyTextPx : menu_width_deg;
}

// ---- The view both eyes share ----------------------------------------------------------
// Quest 3 over Link (fovMutable false) gives each eye about L-54 R+40 U+44 D-55 degrees (the right
// eye mirrored): only +-40 degrees across are seen by both eyes. A canvas (a menu, a dialog or
// note, the travel list) keeps its corners within corner_yaw_deg / corner_pitch_deg of its centre
// (vr.ini ui_menu_corner_yaw_deg / ui_menu_corner_pitch_deg): inside that shared view with a
// margin, never at the edge of one eye's view, and read with the eyes turned at most that far.
// The angles are view angles (the atan of the view-plane tangent, as a headset's FOV is given),
// seen from the canvas's origin looking at its centre.
constexpr float SharedViewHalfDeg = 40.0f;
constexpr float CornerYawDefaultDeg = 34.0f;
constexpr float CornerPitchDefaultDeg = 30.0f;

struct ViewAngles {
    float yaw_deg{};   // + = right
    float pitch_deg{}; // + = up
};

// A canvas piece's corner seen from its origin, looking at the piece's centre: `width_deg`
// across (a cylinder's central angle, or a flat quad that wide in angle) and `height_deg` tall
// as the canvas measures it (stage px times the same deg per px: height / radius for a
// cylinder). A cylinder's corners are further up than its middle column's top (they are
// nearer to the eye's view plane).
inline ViewAngles CanvasCorner(float width_deg, float height_deg, bool curved) {
    const float half_w = 0.5f * std::max(width_deg, 0.0f) * DegToRadF;
    const float tan_x = std::tan(std::min(half_w, 1.5f));
    const float tan_y = curved ? 0.5f * std::max(height_deg, 0.0f) * DegToRadF /
                                     std::max(std::cos(half_w), 1e-3f)
                               : tan_x * height_deg / std::max(width_deg, 1e-3f);
    return {std::atan(tan_x) / DegToRadF, std::atan(tan_y) / DegToRadF};
}

// The largest deg per stage px at which a `width_px` x `height_px` crop keeps its corners
// within corner_yaw_deg / corner_pitch_deg (CanvasCorner).
inline float SharedViewDegPerPx(float width_px, float height_px, bool curved,
                                float corner_yaw_deg = CornerYawDefaultDeg,
                                float corner_pitch_deg = CornerPitchDefaultDeg) {
    const float w = std::max(width_px, 1.0f);
    const float h = std::max(height_px, 1.0f);
    const float yaw = std::clamp(corner_yaw_deg, 5.0f, 80.0f);
    const float pitch = std::clamp(corner_pitch_deg, 5.0f, 80.0f);
    float hi = 2.0f * yaw / w; // the corners' yaw is exactly half the width
    if (CanvasCorner(w * hi, h * hi, curved).pitch_deg <= pitch) {
        return hi;
    }
    float lo = 0.0f; // the corners' pitch grows with the size: bisect
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        (CanvasCorner(w * mid, h * mid, curved).pitch_deg <= pitch ? lo : hi) = mid;
    }
    return lo;
}

// ---- The HUD band's lazy follow ("tag-along") -------------------------------------------
struct Vec3f {
    float x{}, y{}, z{};
};

struct FollowParams {
    // The anchor stays while the head is within this of it: wider than the belt's content
    // (+-21 deg), so looking at any of it never moves it.
    float yaw_dead_deg{28.0f};
    float pitch_dead_deg{15.0f}; // looking up (a tall boss) beyond this brings it up
    // ... and looking down beyond this (below the status clusters' lowest content, -24 deg):
    // reading the HUD never moves it.
    float pitch_dead_down_deg{32.0f};
    float settle_deg{3.0f};      // a chase stops this close to the head
    float hard_deg{45.0f};       // never further than this from the head (fast turns drag it)
    float tau_s{0.3f};           // chase time constant
    float origin_tau_s{0.2f};    // head position smoothing
    float pitch_min_deg{-50.0f}; // the anchor's pitch range
    float pitch_max_deg{50.0f};
};

struct FollowState {
    bool valid{false};
    float yaw{};   // radians, tracking space, + = left
    float pitch{}; // radians, + = up
    Vec3f origin{};
    bool chase_yaw{false};
    bool chase_pitch{false};
};

// Wraps an angle difference into [-pi, pi].
inline float WrapRad(float a) {
    constexpr float Pi = 3.14159265f;
    a = std::fmod(a + Pi, 2.0f * Pi);
    if (a < 0.0f) {
        a += 2.0f * Pi;
    }
    return a - Pi;
}

// One step of the band anchor toward the head (yaw/pitch in radians, tracking space). Idle while
// the head is within the dead zone (pitch: pitch_dead_deg above, pitch_dead_down_deg below);
// beyond it, eases toward the head until within settle_deg; never more than hard_deg behind. The
// origin follows the head position smoothed. No roll. Returns true when the anchor moved by more
// than a hundredth of a degree.
inline bool StepHudFollow(FollowState& st, float head_yaw, float head_pitch, const Vec3f& head_pos,
                          float dt, const FollowParams& p) {
    const float pitch_min = p.pitch_min_deg * DegToRadF;
    const float pitch_max = p.pitch_max_deg * DegToRadF;
    const float target_pitch = std::clamp(head_pitch, pitch_min, pitch_max);
    if (!st.valid) {
        st = {true, head_yaw, target_pitch, head_pos, false, false};
        return true;
    }
    const FollowState before = st;
    const float k = p.tau_s > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / p.tau_s) : 1.0f;
    // Yaw.
    float dyaw = WrapRad(head_yaw - st.yaw);
    if (std::abs(dyaw) > p.yaw_dead_deg * DegToRadF) {
        st.chase_yaw = true;
    }
    if (st.chase_yaw) {
        st.yaw = WrapRad(st.yaw + dyaw * k);
        dyaw = WrapRad(head_yaw - st.yaw);
        if (std::abs(dyaw) <= p.settle_deg * DegToRadF) {
            st.chase_yaw = false;
        }
    }
    const float hard = p.hard_deg * DegToRadF;
    if (std::abs(dyaw) > hard) {
        st.yaw = WrapRad(head_yaw - (dyaw > 0.0f ? hard : -hard));
    }
    // Pitch (toward the clamped head pitch).
    float dpitch = target_pitch - st.pitch;
    if (dpitch > p.pitch_dead_deg * DegToRadF || -dpitch > p.pitch_dead_down_deg * DegToRadF) {
        st.chase_pitch = true;
    }
    if (st.chase_pitch) {
        st.pitch += dpitch * k;
        dpitch = target_pitch - st.pitch;
        if (std::abs(dpitch) <= p.settle_deg * DegToRadF) {
            st.chase_pitch = false;
        }
    }
    if (std::abs(dpitch) > hard) {
        st.pitch = target_pitch - (dpitch > 0.0f ? hard : -hard);
    }
    st.pitch = std::clamp(st.pitch, pitch_min, pitch_max);
    // Origin.
    const float ko =
        p.origin_tau_s > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / p.origin_tau_s) : 1.0f;
    st.origin.x += (head_pos.x - st.origin.x) * ko;
    st.origin.y += (head_pos.y - st.origin.y) * ko;
    st.origin.z += (head_pos.z - st.origin.z) * ko;
    return std::abs(WrapRad(st.yaw - before.yaw)) > 0.01f * DegToRadF ||
           std::abs(st.pitch - before.pitch) > 0.01f * DegToRadF;
}

// ---- A small dialog's crop ---------------------------------------------------------------
// The canvas shows a dialog or note as a crop around its box (band px), locked once the box held
// still for `still_frames` + 1 UI frames of the dialog's own class (the box fades in, a prompt
// fades out), or after `timeout_s` on whatever box was seen last (an animated element never holds
// it open), or on DialogCrop without any box. After the lock it only grows, symmetrically about
// its centre (more lines), so nothing on it ever moves.
struct CropState {
    bool valid{false};
    float cx{}, cy{}, hw{}, hh{};
    bool have_box{false};
    std::array<s32, 4> last{};
    u32 still{0};
    float waited_s{0.0f};
};

// One step: `box` (band px, end exclusive, valid) from a UI frame of the dialog's class with
// sequence `fresh` (false: no new box this step), `margin` band px around it, `dt` seconds since
// the last step. Returns true when the crop was placed or grew.
inline bool StepDialogCrop(CropState& st, bool fresh, bool box_valid, const std::array<s32, 4>& box,
                           float margin, float dt, const StageRect& fallback, float band_scale,
                           u32 still_frames = 2, float timeout_s = 0.4f) {
    st.waited_s += std::max(dt, 0.0f);
    const auto place = [&](float x0, float y0, float x1, float y1) {
        st.valid = true;
        st.cx = 0.5f * (x0 + x1);
        st.cy = 0.5f * (y0 + y1);
        st.hw = 0.5f * (x1 - x0);
        st.hh = 0.5f * (y1 - y0);
    };
    if (fresh && box_valid) {
        const float x0 = static_cast<float>(box[0]) - margin;
        const float y0 = static_cast<float>(box[1]) - margin;
        const float x1 = static_cast<float>(box[2]) + margin;
        const float y1 = static_cast<float>(box[3]) + margin;
        if (!st.valid) {
            const bool same = st.have_box && std::abs(box[0] - st.last[0]) <= 2 &&
                              std::abs(box[1] - st.last[1]) <= 2 &&
                              std::abs(box[2] - st.last[2]) <= 2 &&
                              std::abs(box[3] - st.last[3]) <= 2;
            st.still = same ? st.still + 1 : 0;
            st.last = box;
            st.have_box = true;
            if (st.still >= still_frames || st.waited_s >= timeout_s) {
                place(x0, y0, x1, y1);
                return true;
            }
            return false;
        }
        const float hw = std::max(st.cx - x0, x1 - st.cx);
        const float hh = std::max(st.cy - y0, y1 - st.cy);
        if (hw > st.hw + 0.5f || hh > st.hh + 0.5f) {
            st.hw = std::max(st.hw, hw);
            st.hh = std::max(st.hh, hh);
            return true;
        }
        return false;
    }
    if (!st.valid && st.waited_s >= timeout_s) {
        if (st.have_box) {
            place(static_cast<float>(st.last[0]) - margin, static_cast<float>(st.last[1]) - margin,
                  static_cast<float>(st.last[2]) + margin, static_cast<float>(st.last[3]) + margin);
        } else {
            place(fallback.x0 * band_scale, fallback.y0 * band_scale, fallback.x1 * band_scale,
                  fallback.y1 * band_scale);
        }
        return true;
    }
    return false;
}

// ---- A full menu's crop (ui_menu_fit) ----------------------------------------------------
// A full-screen menu's content box is measured over the whole band in the menu's own frames, with
// the gameplay HUD's footprint in the status clusters left out (the game draws its HUD in the
// corners of some menus: the travel list, the Options grid). The canvas shows the crop around it
// as large as fits: the crop's angular size at most max_width_deg x max_height_deg and its body
// text at most max_em_deg, never smaller than the whole menu at full_width_deg (a menu that fills
// the screen keeps today's size).
inline float MenuDegPerPx(const StageRect& crop, float max_width_deg, float max_height_deg,
                          float max_em_deg, float full_width_deg) {
    const float fit = std::min({max_width_deg / std::max(crop.Width(), 1.0f),
                                max_height_deg / std::max(crop.Height(), 1.0f),
                                max_em_deg / BodyTextPx});
    return std::max(fit, full_width_deg / StageWidth);
}

// ---- A small dialog's size (ui_dialog_max_deg) -----------------------------------------------
// The game's small boxes are wide strips around a line or two of text: the lower-centre object
// message (IngameMessageBox: "No place to awaken.", a door's "Closed", the Doll's "An abandoned
// doll."; its crop 1017 x 329 stage px with the margin), the centre YES / NO box (MessageBox:
// "Return to the Hunter's Dream?", 1118 x 286), an NPC's talk choices (1016 x 329), a note's window
// (~1100). At the dialogs' text size alone (ui_dialog_em_deg, 1.3 deg) they came out 55-61 deg
// across, half the field of view (playtest 2026-10-08: "the tooltip text ... looked ENORMOUS"; the
// HUD's pickup pop-up of the same style is 1.0-deg text low in the view). So the box is at most
// `max_width_deg` across (vr.ini ui_dialog_max_deg, 34: about a third of the view, the text of
// these boxes 0.73-0.80 deg, near the full menus' 0.85-0.92) and its text at most `em_deg` (a
// narrow box keeps ui_dialog_em_deg). The cap gives way only to DialogMinEmDeg: a crop wider than
// any of the game's boxes (measured with something across the screen under it: playtest
// 2026-10-08, a message opened in the Dream as a banner's dim faded, 1488 x 409 stage px) keeps its
// text readable rather than its width.
constexpr float DialogMaxDegDefault = 34.0f;
constexpr float DialogMinEmDeg = 0.7f;

// The deg per stage px of a dialog's crop `width_px` (stage px) wide: text `em_deg` (24 stage px)
// at most, the crop `max_width_deg` across at most (<= 0: no cap), the text DialogMinEmDeg at
// least where the cap would make it smaller (and never more than em_deg).
inline float DialogDegPerPx(float width_px, float em_deg, float max_width_deg) {
    const float em = em_deg / BodyTextPx;
    if (max_width_deg <= 0.0f) {
        return em;
    }
    const float cap = max_width_deg / std::max(width_px, 1.0f);
    const float floor = std::min(DialogMinEmDeg, em_deg) / BodyTextPx;
    return std::min(em, std::max(cap, floor));
}

// Where a canvas piece's content is placed (the presenter's canvas): its angular scale.
enum class CanvasKind : u8 {
    Menu,   // a full menu cropped to its content (ui_menu_fit): MenuDegPerPx
    Dialog, // a small dialog or note: DialogDegPerPx
    Whole,  // a full menu whole (ui_menu_fit=0): full_width_deg across
};

struct CanvasLimits {
    float max_width_deg{0.0f};  // ui_menu_max_width_deg (0: the corners decide)
    float max_height_deg{0.0f}; // ui_menu_max_height_deg (0: the corners decide)
    float max_em_deg{1.3f};     // ui_dialog_em_deg: a dialog's text at most, a menu's largest
    float dialog_max_width_deg{DialogMaxDegDefault}; // ui_dialog_max_deg (0: no cap)
    float full_width_deg{68.0f}; // ui_menu_width_deg: a whole menu, a cropped one's smallest
    float corner_yaw_deg{CornerYawDefaultDeg};
    float corner_pitch_deg{CornerPitchDefaultDeg};
    bool curved{true};
};

// The deg per stage px of a canvas crop `rect` (stage px) of `kind`: as MenuDegPerPx, the dialog's
// (DialogDegPerPx) or the whole-menu width, then never past the shared view's corners (the floors
// and text sizes give way to it).
inline float CanvasDegPerPx(const StageRect& rect, CanvasKind kind, const CanvasLimits& l) {
    float dpp{};
    switch (kind) {
    case CanvasKind::Menu:
        dpp = MenuDegPerPx(rect, l.max_width_deg > 0.0f ? l.max_width_deg : 360.0f,
                           l.max_height_deg > 0.0f ? l.max_height_deg : 360.0f, l.max_em_deg,
                           l.full_width_deg);
        break;
    case CanvasKind::Dialog:
        dpp = DialogDegPerPx(rect.Width(), l.max_em_deg, l.dialog_max_width_deg);
        break;
    case CanvasKind::Whole:
        dpp = l.full_width_deg / StageWidth;
        break;
    }
    return std::min(dpp, SharedViewDegPerPx(rect.Width(), rect.Height(), l.curved,
                                            l.corner_yaw_deg, l.corner_pitch_deg));
}

// The crop locks like a dialog's (the box held still for still_frames + 1 frames, or timeout_s on
// the newest box; no box at all: the whole band); given `opaque_box` (the frame's opaque UI's own
// box, VrUiLayer::ContentBox::opaque_box), still also means that box is there and held still:
// the game fades a menu in while it slides it into place, and its slide can pause for three
// frames while the menu is still translucent (mock menufix/runs/t2: the Options grid locked at
// x1 980 of 990 stage px, then followed the slide in three steps). Then it grows at once with new
// content outside it (a sub-page, a box over a list), and shrinks to new, smaller content (the
// Options grid again after the Inventory) once every box for shrink_s fitted in a crop at most
// shrink_area of the current one: to the union of those boxes, so a blinking element is never cut.
// A page much smaller than the crop (at most `quick_area` of it: the grid again after the Inventory
// or Status, 17% of it) is taken as soon as it holds still like at the lock (still_frames + 1 equal
// boxes in a row, the game's own slide-in over), not 0.6 s later (ui_menu_snap; 0 = off). With
// `grow_frames` 2 (ui_menu_snap) a growth needs two of the menu's frames in a row reaching out of
// the crop, so a frame of a fade or of a list closing never moves it (mock menufix/runs/d1: Leave
// on the Doll's list grew it down by 190 stage px, the list jumping 5 degrees up, two frames
// before it closed; runs/t3: one frame of the quantity box's dim fading out, reaching only the
// left edge, grew the Inventory's to the whole band); MenuCropState::grow_seen is reset by the
// caller for a frame it does not take. Given `opaque_box`, a screen-wide box before the lock (a
// dim the menu fades in behind itself: Level Up's (0, 186)..(2720, 1530), runs/d2) counts as the
// menu's opaque UI with the last box before it that was not screen-wide, when that opaque UI is
// not screen-wide itself: the crop locks on the book (68 x 35.8 degrees, text 0.94), not on the
// dim (68 x 34.5, text 0.85, the book smaller). With `grow_slack` (a share of the margin; 0.5 with
// ui_menu_snap) a placed crop grows only once the content comes within that much of the margin of
// its edge, never cut: a page fading in widens its box by a few px as its soft edges cross the
// alpha threshold (mock menufix/runs/t3: the Inventory's crop moved 4 and 5 stage px 117 and 134
// ms after it grew), and that no longer moves it. And with quick_area the menu's first page again
// (the Options grid back from the Inventory or Status), inside the crop the menu locked on
// (`root`) and at most quick_area of the current one, takes that crop on its first frame: the
// grid comes back in place at once, its own short slide-in playing on it as on the flat screen
// (runs/d1: the grid came back 0.23 s after its first frame, small and off to the left in the
// Status crop until then).
constexpr float MenuQuickShrinkArea = 0.5f;
// The opening/closing slide may use the padding without moving the canvas.
// Grow when content reaches outside the crop, not when padding alone does.
constexpr float MenuGrowSlack = 1.0f;
struct MenuCropState {
    bool valid{false};
    float x0{}, y0{}, x1{}, y1{}; // band px
    bool have_box{false};
    std::array<s32, 4> last{};
    u32 still{0};
    float waited_s{0.0f};
    bool shrinking{false};
    float shrink_s{0.0f};
    std::array<float, 4> shrink_union{};
    std::array<s32, 4> shrink_last{};  // the last box while shrinking, and how many in a row
    u32 shrink_still{0};               // were the same (the quick shrink)
    std::array<s32, 4> last_opaque{};  // the opaque UI's box of the last frame looked at
    u32 grow_seen{0};                  // frames in a row reaching out of the crop (grow_frames)
    std::array<float, 4> grow_union{}; // ... and their union (band px, with the margin)
    // A 30 Hz UI animation can repeat across several 90/120 Hz rendered frames.
    // Keep a time-qualified reference box; pairwise NearBox checks can drift.
    float still_s{0.0f};
    std::array<s32, 4> still_box{}, still_opaque{};
    bool root_valid{false};            // the crop the menu locked on (its first page in place)
    std::array<float, 4> root{};
    bool have_narrow{false};           // the last box before the lock that was not screen-wide
    std::array<s32, 4> narrow{};
};

// A box spanning the band's whole width (within ScreenWideEdge of both sides): a screen-wide
// layer over the menu, not its content. The game's menus keep a margin (the inventory, the widest,
// starts 99 px in on a 1920 px band), but a dim over everything does not: the shop's quantity box,
// a sub-page's fade, an NPC's subtitle band as a talk list closes (mock 2026-10-07: each one grew
// the crop to the whole band, the list's text 0.93 -> 0.85 deg until it shrank back).
constexpr float ScreenWideEdge = 0.02f;
inline bool IsScreenWideBox(const std::array<s32, 4>& box, float band_w) {
    const float edge = ScreenWideEdge * band_w;
    return static_cast<float>(box[0]) <= edge && static_cast<float>(box[2]) >= band_w - edge;
}

// Two boxes (band px) within 2 px of each other on every side.
inline bool NearBox(const std::array<s32, 4>& a, const std::array<s32, 4>& b) {
    return std::abs(a[0] - b[0]) <= 2 && std::abs(a[1] - b[1]) <= 2 && std::abs(a[2] - b[2]) <= 2 &&
           std::abs(a[3] - b[3]) <= 2;
}

// Menu transitions are hidden while the game's slide/fade is in flight, then
// shown at full opacity. This gate never stalls the world or controller input.
struct MenuPopState {
    bool pending{}, have_box{};
    float elapsed{}, still_s{};
    std::array<s32, 4> box{}, opaque{};
};
inline void BeginMenuPop(MenuPopState& st) {
    st = {};
    st.pending = true;
}
inline bool StepMenuPop(MenuPopState& st, bool fresh, bool valid,
                        const std::array<s32, 4>& box, const std::array<s32, 4>& opaque,
                        float dt) {
    if (!st.pending) return true;
    dt = std::max(dt, 0.0f);
    st.elapsed += dt;
    st.still_s += dt;
    if (fresh && valid) {
        const bool same = st.have_box && NearBox(box, st.box) && NearBox(opaque, st.opaque);
        if (!same) {
            st.still_s = 0.0f;
            st.box = box;
            st.opaque = opaque;
        }
        st.have_box = true;
        const bool solid = opaque[2] > opaque[0] && opaque[3] > opaque[1];
        if (st.elapsed >= 0.15f && st.still_s >= 0.06f && solid) st.pending = false;
    }
    // An ignored button or unavailable measurement must not hide the UI indefinitely.
    if (st.elapsed >= 0.6f) st.pending = false;
    return !st.pending;
}

// A placed crop's growth test: `box` (band px) with (1 - grow_slack) of the margin around it
// reaches out of the crop (StepMenuCrop grows on it).
inline bool MenuBoxOutOfCrop(const MenuCropState& st, const std::array<s32, 4>& box, float margin,
                             float grow_slack, float band_w, float band_h) {
    const auto clamp_x = [&](float v) { return std::clamp(v, 0.0f, band_w); };
    const auto clamp_y = [&](float v) { return std::clamp(v, 0.0f, band_h); };
    const float reach = (1.0f - std::clamp(grow_slack, 0.0f, 1.0f)) * margin;
    return clamp_x(static_cast<float>(box[0]) - reach) < st.x0 - 0.5f ||
           clamp_y(static_cast<float>(box[1]) - reach) < st.y0 - 0.5f ||
           clamp_x(static_cast<float>(box[2]) + reach) > st.x1 + 0.5f ||
           clamp_y(static_cast<float>(box[3]) + reach) > st.y1 + 0.5f;
}

// One step: `box` (band px, end exclusive) from a frame of the menu (`fresh`: a new one, of the
// menu's class, with the menu drawn), `margin` band px around it, the band's size, `dt` seconds
// since the last step. Returns true when the crop was placed, grew or shrank.
inline bool StepMenuCrop(MenuCropState& st, bool fresh, bool box_valid,
                         const std::array<s32, 4>& box_in, float margin, float band_w, float band_h,
                         float dt, u32 still_frames = 2, float timeout_s = 0.4f,
                         float shrink_after_s = 0.6f, float shrink_area = 0.8f,
                         float quick_area = 0.0f, const std::array<s32, 4>* opaque_box = nullptr,
                         u32 grow_frames = 1, float grow_slack = 0.0f,
                         float settle_s = 0.0f) {
    std::array<s32, 4> box = box_in;
    dt = std::max(dt, 0.0f);
    st.waited_s += dt;
    st.still_s += dt;
    // This frame's opaque UI as the last frame's (and there at all), when it is measured.
    bool opaque_still = true;
    if (opaque_box != nullptr && fresh && box_valid) {
        const auto& o = *opaque_box;
        opaque_still = o[2] > o[0] && o[3] > o[1] && NearBox(o, st.last_opaque);
        st.last_opaque = o;
    }
    const auto clamp_x = [&](float v) { return std::clamp(v, 0.0f, band_w); };
    const auto clamp_y = [&](float v) { return std::clamp(v, 0.0f, band_h); };
    const auto area = [](float x0, float y0, float x1, float y1) {
        return std::max(x1 - x0, 0.0f) * std::max(y1 - y0, 0.0f);
    };
    const auto place = [&](float x0, float y0, float x1, float y1) {
        st.valid = true;
        st.x0 = clamp_x(x0);
        st.y0 = clamp_y(y0);
        st.x1 = std::max(clamp_x(x1), st.x0 + 1.0f);
        st.y1 = std::max(clamp_y(y1), st.y0 + 1.0f);
        st.shrinking = false;
    };
    if (fresh && box_valid && !st.valid && opaque_box != nullptr) {
        // Before the lock: a screen-wide dim behind the menu is not its content (see above).
        const auto& o = *opaque_box;
        if (!IsScreenWideBox(box_in, band_w)) {
            st.narrow = box_in;
            st.have_narrow = true;
        } else if (o[2] > o[0] && o[3] > o[1] && !IsScreenWideBox(o, band_w)) {
            box = o;
            if (st.have_narrow) {
                box = {std::min(o[0], st.narrow[0]), std::min(o[1], st.narrow[1]),
                       std::max(o[2], st.narrow[2]), std::max(o[3], st.narrow[3])};
            }
        }
    }
    if (fresh && box_valid) {
        const float x0 = clamp_x(static_cast<float>(box[0]) - margin);
        const float y0 = clamp_y(static_cast<float>(box[1]) - margin);
        const float x1 = clamp_x(static_cast<float>(box[2]) + margin);
        const float y1 = clamp_y(static_cast<float>(box[3]) + margin);
        if (!st.valid) {
            const bool same = st.have_box && opaque_still &&
                              NearBox(box, settle_s > 0.0f ? st.still_box : st.last) &&
                              (settle_s <= 0.0f || opaque_box == nullptr ||
                               NearBox(*opaque_box, st.still_opaque));
            st.still = same ? st.still + 1 : 0;
            if (!same) {
                st.still_s = 0.0f;
                st.still_box = box;
                st.still_opaque = opaque_box != nullptr ? *opaque_box : std::array<s32, 4>{};
            }
            st.last = box;
            st.have_box = true;
            if ((st.still >= still_frames && st.still_s >= settle_s) ||
                st.waited_s >= timeout_s) {
                place(x0, y0, x1, y1);
                st.root_valid = true;
                st.root = {st.x0, st.y0, st.x1, st.y1};
                return true;
            }
            return false;
        }
        // Out of the crop: the content with (1 - grow_slack) of the margin around it.
        if (MenuBoxOutOfCrop(st, box, margin, grow_slack, band_w, band_h)) {
            auto& g = st.grow_union;
            g = st.grow_seen == 0 ? std::array<float, 4>{x0, y0, x1, y1}
                                  : std::array<float, 4>{std::min(g[0], x0), std::min(g[1], y0),
                                                         std::max(g[2], x1), std::max(g[3], y1)};
            if (++st.grow_seen < grow_frames) {
                return false;
            }
            st.grow_seen = 0;
            place(std::min(st.x0, g[0]), std::min(st.y0, g[1]), std::max(st.x1, g[2]),
                  std::max(st.y1, g[3]));
            return true;
        }
        st.grow_seen = 0;
        const auto& r = st.root;
        if (quick_area > 0.0f && st.root_valid && static_cast<float>(box[0]) >= r[0] &&
            static_cast<float>(box[1]) >= r[1] && static_cast<float>(box[2]) <= r[2] &&
            static_cast<float>(box[3]) <= r[3] &&
            area(r[0], r[1], r[2], r[3]) <= quick_area * area(st.x0, st.y0, st.x1, st.y1)) {
            place(r[0], r[1], r[2], r[3]); // the first page again: where it was, at once
            return true;
        }
        if (area(x0, y0, x1, y1) > shrink_area * area(st.x0, st.y0, st.x1, st.y1)) {
            st.shrinking = false;
            return false;
        }
        if (!st.shrinking) {
            st.shrinking = true;
            st.shrink_s = 0.0f;
            st.shrink_union = {x0, y0, x1, y1};
            st.shrink_still = 0;
        } else {
            auto& u = st.shrink_union;
            u = {std::min(u[0], x0), std::min(u[1], y0), std::max(u[2], x1), std::max(u[3], y1)};
            if (area(u[0], u[1], u[2], u[3]) > shrink_area * area(st.x0, st.y0, st.x1, st.y1)) {
                st.shrinking = false; // together they are not smaller
                return false;
            }
            st.shrink_still =
                NearBox(box, st.shrink_last) && opaque_still ? st.shrink_still + 1 : 0;
        }
        st.shrink_last = box;
        const auto& u = st.shrink_union;
        if (quick_area > 0.0f && st.shrink_still >= still_frames &&
            area(u[0], u[1], u[2], u[3]) <= quick_area * area(st.x0, st.y0, st.x1, st.y1)) {
            place(u[0], u[1], u[2], u[3]); // a much smaller page, held still: at once
            return true;
        }
    }
    if (st.valid && st.shrinking) {
        st.shrink_s += dt;
        if (st.shrink_s >= shrink_after_s) {
            const auto u = st.shrink_union;
            place(u[0], u[1], u[2], u[3]);
            return true;
        }
        return false;
    }
    if (!st.valid && st.waited_s >= timeout_s) {
        if (st.have_box) {
            place(static_cast<float>(st.last[0]) - margin, static_cast<float>(st.last[1]) - margin,
                  static_cast<float>(st.last[2]) + margin, static_cast<float>(st.last[3]) + margin);
            st.root_valid = true;
            st.root = {st.x0, st.y0, st.x1, st.y1};
        } else {
            place(0.0f, 0.0f, band_w, band_h); // never measured: the whole menu
        }
        return true;
    }
    return false;
}

// Whether StepMenuCrop takes this frame's box: always until the crop is placed; after that a
// screen-wide box only with `screen_wide_grows` (vr.ini ui_menu_fit_screen_wide) or when it is a
// page (`page`: StepScreenWidePage).
inline bool MenuCropTakesBox(const MenuCropState& st, bool box_valid, const std::array<s32, 4>& box,
                             float band_w, bool screen_wide_grows, bool page = false) {
    return !st.valid || !box_valid || screen_wide_grows || page || !IsScreenWideBox(box, band_w);
}

// ---- A page over the whole screen vs a dim over the menu (ui_menu_page) -------------------------
// Status, opened from the Options grid, covers the whole screen with its book: a screen-wide box,
// like a dim's, but content. MenuCropTakesBox kept the grid's crop for it, so Status was shown cut
// down to the grid's 39 x 27 degrees, most of it missing (playtest 2026-10-09: "the status menu
// is broken"; mock menufix/runs/t1: the box (0, 0)..(2720, 1530) band px, 77% of the band opaque
// UI, the opaque UI's own box the whole band, the crop the grid's (262, 395)..(990, 895) stage).
// A dim over the menu is translucent: what is opaque under it is the menu inside the crop and a
// small box (the quantity box), or a line of subtitle text. So a screen-wide box is a page when at
// least PageOpaqueMin of the band is opaque UI (alpha > 250/255, VrUiLayer::ContentBox::opaque)
// and the opaque UI's own box reaches outside the placed crop over at least PageOutsideMin of the
// band, in PageFrames fresh frames in a row (the game's cross-fade between pages is never one).
constexpr float PageOpaqueMin = 0.30f;
constexpr float PageOutsideMin = 0.20f;
constexpr u32 PageFrames = 2;

// This frame's measurement alone: `opaque_box` (band px, end exclusive; empty: none), `opaque_px`
// band px of opaque UI, against the placed crop `st`.
inline bool IsScreenWidePage(const MenuCropState& st, const std::array<s32, 4>& opaque_box,
                             u32 opaque_px, float band_w, float band_h) {
    const float band = band_w * band_h;
    if (!st.valid || band <= 0.0f || static_cast<float>(opaque_px) < PageOpaqueMin * band ||
        opaque_box[2] <= opaque_box[0] || opaque_box[3] <= opaque_box[1]) {
        return false;
    }
    const float x0 = static_cast<float>(opaque_box[0]);
    const float y0 = static_cast<float>(opaque_box[1]);
    const float x1 = static_cast<float>(opaque_box[2]);
    const float y1 = static_cast<float>(opaque_box[3]);
    const float inside = std::max(std::min(x1, st.x1) - std::max(x0, st.x0), 0.0f) *
                         std::max(std::min(y1, st.y1) - std::max(y0, st.y0), 0.0f);
    return (x1 - x0) * (y1 - y0) - inside >= PageOutsideMin * band;
}

// One step with this frame's box (`fresh`: a new one of the menu's frames): true once
// IsScreenWidePage held for PageFrames fresh frames in a row (MenuCropTakesBox's `page`).
struct PageRun {
    u32 frames{0};
};
inline bool StepScreenWidePage(PageRun& run, bool fresh, bool page) {
    if (fresh) {
        run.frames = page ? run.frames + 1 : 0;
    }
    return run.frames >= PageFrames;
}

// ---- The Options grid's pages, known from the game (ui_menu_page, ui_menu_snap) ----------------
// What the game says about the open menu (VR::GameMenuGridCell: the menu's name from its Open, the
// cursor from the grid's probe): the Options grid (IngameTop) and the cell its cursor is on (0
// Inventory, 1 Stats, 2 System, 3 VR, ...); GridCursorUnknown without the probe (vr_menu=0);
// GridNone for every other menu (and for the grid after a menu opened over it: the name is then
// the newer menu's).
constexpr s32 GridNone = -2;
constexpr s32 GridCursorUnknown = -1;
constexpr s32 GridCellStats = 1;
constexpr s32 GridCellSystem = 2;

// A box over the whole band (within ScreenWideEdge of each of its four sides).
inline bool IsWholeBandBox(const std::array<s32, 4>& box, float band_w, float band_h) {
    const float edge = ScreenWideEdge * band_h;
    return IsScreenWideBox(box, band_w) && static_cast<float>(box[1]) <= edge &&
           static_cast<float>(box[3]) >= band_h - edge;
}

// Status from the grid. The game cross-fades the grid into the book in about 5 of its frames
// (0.17 s). Every one of them is a box over the whole band (the book's backdrop, faint), but only
// 1-8% of the band is opaque, like a dim (mock menufix/runs/f3, v1: 3.8, 1.1, 4.8, 2.0, 8.2%, then
// the book at 64%). IsScreenWidePage takes it only once the book is opaque, so the cross-fade
// played in the grid's crop for 166-235 ms (the middle of the book 1.5x too large), then the crop
// jumped to the whole band. With the grid's cursor on Stats, the first box over
// the whole band is the page and the crop takes the band at once. The box is measured a frame
// late, so the grid's crop still shows the cross-fade's first frame, the grid nearly whole; from
// the next frame on, the grid fading out and the book fading in, everything is at Status's size.
// Without the cursor (GridCursorUnknown) this holds only while the crop is the one the grid locked
// on. A crop that already is the whole band has nothing to take.
inline bool IsGridPageAtOnce(const MenuCropState& st, s32 grid_cell, bool box_valid,
                             const std::array<s32, 4>& box, float band_w, float band_h) {
    if (!st.valid || !box_valid || grid_cell == GridNone || !IsWholeBandBox(box, band_w, band_h)) {
        return false;
    }
    if (st.x0 <= 0.5f && st.y0 <= 0.5f && st.x1 >= band_w - 0.5f && st.y1 >= band_h - 0.5f) {
        return false;
    }
    if (grid_cell == GridCellStats) {
        return true;
    }
    const auto& r = st.root;
    return grid_cell == GridCursorUnknown && st.root_valid && std::abs(st.x0 - r[0]) <= 0.5f &&
           std::abs(st.y0 - r[1]) <= 0.5f && std::abs(st.x1 - r[2]) <= 0.5f &&
           std::abs(st.y1 - r[3]) <= 0.5f;
}

// System from the grid. Its panel comes in over the grid's fading frames, and settles higher and
// wider only after the game's pause while its list loads (0.23 s): stage (437, 247)..(1477, 815).
// Mock menufix/runs/v1, band 2720 x 1530: the first frames (448, 406)..(2038, 1234) to (439,
// 389)..(2075, 1234) band px with the grid in them, then the panel alone at (619, 350)..(2092,
// 1155). The crop grew to the first frames, then again to the settled panel, 0.35 s after the
// open, and the panel moved about 1 degree (66.0 x 34.8 -> 67.1 x 36.4 degrees, the same text).
// With the grid's cursor on System, a frame that grows the crop also takes in the settled panel,
// so the crop grows once. A panel that settles elsewhere still grows the crop, as before; it is
// never cut.
constexpr StageRect GridSystemPanel{437.0f, 247.0f, 1477.0f, 815.0f};

// `box` (band px) joined with the settled page the grid's cursor opens (`k` band px per stage px),
// for a frame whose box grows the placed crop (MenuBoxOutOfCrop with `margin` and `grow_slack`, as
// StepMenuCrop is called); `box` itself otherwise.
inline std::array<s32, 4> GridPageGrowthBox(const MenuCropState& st, s32 grid_cell,
                                            const std::array<s32, 4>& box, float margin,
                                            float grow_slack, float band_w, float band_h, float k) {
    if (!st.valid || grid_cell != GridCellSystem ||
        !MenuBoxOutOfCrop(st, box, margin, grow_slack, band_w, band_h)) {
        return box;
    }
    const auto& p = GridSystemPanel;
    return {std::min(box[0], static_cast<s32>(std::floor(p.x0 * k))),
            std::min(box[1], static_cast<s32>(std::floor(p.y0 * k))),
            std::max(box[2], static_cast<s32>(std::ceil(p.x1 * k))),
            std::max(box[3], static_cast<s32>(std::ceil(p.y1 * k)))};
}

// The canvas's pitch where it opens (degrees, + = up): the head's, kept within the menu panel's
// range, then the menu's own offset (the travel list's ui_travel_pitch_deg; 0 for the others).
inline float CanvasAimPitchDeg(float head_pitch_deg, float offset_deg, float min_deg,
                               float max_deg) {
    return std::clamp(head_pitch_deg, min_deg, max_deg) + offset_deg;
}

// ---- The HUD under a full menu (vr.ini ui_menu_hud) ----------------------------------------
// The game draws its HUD under a full menu (the Options grid shows it, the inventory's and the
// Status page's panels hide it) and does not pause: HP, stamina, vials, bullets and the quick
// item, and the echoes / insight, are shown under the menu's canvas, fixed to it, at the
// gameplay HUD's size (deg_per_px): the HP bar's left end (HudLeftContentX) under the canvas's
// left edge and the echoes counter's right end (HudRightContentX) under its right edge, but at
// least hud_corner_yaw_deg out (a narrow menu: where the gameplay HUD is) and, seen from the
// canvas's origin, never further out than menu_corner_yaw_deg (inside the view both eyes
// share; a row far below the centre spreads sideways, so the corner comes in for it); their rows
// like the gameplay HUD's (hud_corner_pitch_deg below the canvas's centre) or lower, the HP bar's
// top MenuHudGapDeg below the canvas's lowest point (its corners, CanvasCorner), so nothing of
// the menu is covered. Angles from the canvas's centre (yaw + = right, pitch + = up); the two
// status segments only.
constexpr float MenuHudGapDeg = 1.0f;

// A point (stage x, y) of a segment shown as a cylinder piece facing its yaw_deg / row_pitch_deg
// around the origin: its view angles from the origin (yaw + = right, pitch + = up).
inline ViewAngles SegmentPointView(const Segment& seg, float x, float y) {
    const float a = (x - 0.5f * (seg.rect.x0 + seg.rect.x1)) * seg.deg_per_px * DegToRadF;
    const float h = (0.5f * (seg.rect.y0 + seg.rect.y1) - y) * seg.deg_per_px * DegToRadF;
    const float p = seg.row_pitch_deg * DegToRadF;
    const float t = seg.yaw_deg * DegToRadF;
    const float px = std::sin(a);
    const float pz = -std::cos(a);
    const float y1 = h * std::cos(p) - pz * std::sin(p);
    const float z1 = h * std::sin(p) + pz * std::cos(p);
    const float x2 = px * std::cos(t) - z1 * std::sin(t);
    const float z2 = px * std::sin(t) + z1 * std::cos(t);
    const float depth = std::max(-z2, 1e-3f);
    return {std::atan(x2 / depth) / DegToRadF, std::atan(y1 / depth) / DegToRadF};
}

inline HudSegments MenuHudLayout(float canvas_width_deg, float canvas_height_deg, bool curved,
                                 float deg_per_px, float hud_corner_yaw_deg,
                                 float hud_corner_pitch_deg, float menu_corner_yaw_deg) {
    const float s = std::clamp(deg_per_px, 0.01f, 0.1f);
    const float half_w = 0.5f * std::max(canvas_width_deg, 0.0f);
    const float bottom = CanvasCorner(canvas_width_deg, canvas_height_deg, curved).pitch_deg;
    const float rows_middle = 0.5f * (20.0f + 250.0f); // the status segments' rect (stage y)
    const float rows = std::min(hud_corner_pitch_deg,
                                -bottom - MenuHudGapDeg - (rows_middle - HudContentTopY) * s);
    // The content's outer edges stay within this (a user's wider gameplay HUD keeps its width).
    const float limit = std::max(menu_corner_yaw_deg, hud_corner_yaw_deg);
    float corner = std::max(std::min(half_w, menu_corner_yaw_deg), hud_corner_yaw_deg);
    HudSegments view{};
    for (int i = 0; i < 6; ++i) {
        view = ComputeHudLayout(HudLayout::Belt, s, 0.0f, HudAnchor::View, corner, rows);
        float out = 0.0f; // how far the content's outer edges reach past the limit
        for (const float y : {HudContentTopY, HudContentBottomY}) {
            out = std::max(out, -SegmentPointView(view.segment[0], HudLeftContentX, y).yaw_deg);
            out = std::max(out, SegmentPointView(view.segment[1], HudRightContentX, y).yaw_deg);
        }
        if (out <= limit + 0.01f) {
            break;
        }
        corner = std::max(corner - (out - limit), 5.0f);
    }
    HudSegments result{};
    for (u32 i = 0; i < view.count; ++i) {
        if (!view.segment[i].dynamic && result.count < result.segment.size()) {
            result.segment[result.count++] = view.segment[i];
        }
    }
    return result;
}

} // namespace VR::Ui
