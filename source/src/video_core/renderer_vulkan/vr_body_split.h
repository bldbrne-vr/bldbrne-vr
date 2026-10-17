// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// How one draw of a hunter face set with a VrBodyTable key is recorded (vk_rasterizer_vr_draws
// .cpp): the near-camera cutaway (body_cutaway), the hidden cape (hide_cape) and the head left out
// of the colour passes (body_head_shadow); and with body_mode=arms_only / hands_only what of a
// hunter face set with a VrHandsTable key is drawn (PlanHands). Pure, so the unit tests
// (tests/test_body_split.cpp) cover every combination.
namespace Vulkan::VrBodySplit {

// A key's ranges: keep (the forearms and hands), cut (the rest), cape (partial cape), head (the
// head's triangles), shoulder (the rest of the arms: the upper arms and shoulders); head_only: a
// head part (hat, face, face-gen part, hair, the fc_* bare skin whose arms and hands are keep
// ranges, never shoulder ones), which acts only in a colour pass while the head is hidden by the
// renderer.
struct KeyShape {
    u16 keep{};
    u16 cut{};
    u16 cape{};
    u16 head{};
    bool head_only{};
    u16 shoulder{};
};

struct Frame {
    bool cut_on{};      // this frame's cutaway code is not 0 (first person, body_cutaway)
    bool cape_hidden{}; // hide_cape applies this frame (first person)
    bool head_hidden{}; // body_head_shadow applies this frame (first person, body_mode=no_head)
    // body_cutaway_shoulders: the shoulder ranges fade with the cut ones (the user, 2026-10-08:
    // through the shoulders after a hit or a small head drop); false: they are kept with the arms,
    // exactly the behaviour before 2026-10-09.
    bool shoulder_cut{};
};

enum class Action : u8 {
    Untouched, // not handled here: the draw goes on to the cape table as before
    Skip,      // nothing of it is drawn in this pass
    Whole,     // drawn whole (with the cutaway pipeline when `cut`)
    Split,     // drawn as ranges (VrDrawCapeSplit): keep, shoulder, cut, cape unless hidden, head
               // unless hidden
};

struct Plan {
    Action action{Action::Untouched};
    bool cut{};       // ask for the cutaway pipeline (a colour pass gives it; the cut ranges)
    bool draw_cape{}; // Split: draw the cape ranges
    bool draw_head{}; // Split: draw the head ranges
    bool cut_shoulder{}; // the shoulder ranges are drawn with the cut ones (else with the arms)
};

// Is a draw's pass one the head is left out of: a colour pass whose viewport has the frame's
// aspect (the camera's G-buffer, a reflection of the scene). Bloodborne's shadow cascades also
// write a colour target, but into square tiles, so they draw the head (it casts its shadow);
// depth-only passes always do. `main_aspect` <= 0 (not known yet): every colour pass.
constexpr bool HeadHiddenPass(bool colour, float viewport_w, float viewport_h, float main_aspect) {
    if (!colour) {
        return false;
    }
    if (!(main_aspect > 0.0f)) {
        return true;
    }
    if (!(viewport_w > 0.0f && viewport_h > 0.0f)) {
        return true;
    }
    const float aspect = viewport_w / viewport_h;
    const float d = aspect > main_aspect ? aspect - main_aspect : main_aspect - aspect;
    return d <= 0.02f * main_aspect;
}

// `colour`: the pass the head is left out of (HeadHiddenPass: the camera's G-buffer, reflections);
// the shadow passes draw the head (so it casts its shadow) and keep the whole body.
// `triangles`: the draw is a triangle list (the ranges are triangles of a list).
constexpr Plan PlanDraw(const KeyShape& key, const Frame& frame, bool colour, bool triangles) {
    Plan p{};
    if (key.head_only && (!frame.head_hidden || !colour)) {
        // The head shown, or a shadow pass: a head part is drawn as before body_head_shadow
        // (whole, its own pipeline: the head casts its whole shadow, the cutaway stays off it).
        return p;
    }
    const bool drop_head = frame.head_hidden && colour && key.head != 0;
    const bool drop_cape = frame.cape_hidden && key.cape != 0;
    if (drop_head && key.keep == 0 && key.shoulder == 0 && key.cut == 0 &&
        (key.cape == 0 || drop_cape)) {
        p.action = Action::Skip; // all of it is head (and hidden cape)
        return p;
    }
    if (!triangles || (!frame.cut_on && !drop_head)) {
        return p; // the cutaway is off and the head drawn: as before (the cape table decides)
    }
    // body_cutaway_shoulders: the upper arms and shoulders fade with the rest (a head part never).
    // Off, they are arm triangles (kept), and a face set of only arms (a key only for its shoulder
    // ranges) has nothing to cut: drawn as it is, as before it had a key.
    const bool shoulder_cut = frame.shoulder_cut && frame.cut_on && !key.head_only;
    if (key.shoulder != 0 && !shoulder_cut && key.cut == 0 && key.cape == 0 && key.head == 0) {
        return p;
    }
    // A head part's other triangles (the hat's scarf and clasp, the fc_* bare skin's arms and
    // hands) are drawn with the draw's own pipeline, never faded: as with body_head_shadow=0,
    // where head parts are never cut (the confirmed look: a solid scarf looking down).
    p.cut = frame.cut_on && !key.head_only;
    p.draw_cape = !frame.cape_hidden;
    p.draw_head = !drop_head;
    p.cut_shoulder = shoulder_cut;
    const bool kept = key.keep != 0 || (key.shoulder != 0 && !shoulder_cut);
    p.action = kept || drop_cape || drop_head ? Action::Split : Action::Whole;
    return p;
}

// Is a draw near the first-person camera one of the hunter's own (his model origin known, the VR
// body's anchor)? `rx, ry, rz`: the object transform's translation minus his origin (metres, world
// axes, y up); `upright`: the object's up axis is the world's (a character root: Bloodborne turns
// characters about the vertical only). His skinned parts stand exactly at his origin (measured
// 0.001 m; a frame of lag at a sprint is 11 cm), another character's at its own (at least two
// capsule radii away): a root elsewhere is never his. A rigid piece on one bone (the Foreign
// set's bandage on L_Forearm, a hat on Head) carries that bone's world transform times its
// inverse bind pose: tilted, and its translation within a body length of him (measured 1.6 m for
// the bandage on a raised arm): his within 2.5 m. (A hunter NPC's rigid pieces within 2.5 m are
// taken too; his skinned body never is.)
// A deep duck (the VR body's UpperPos shift past about 0.6 m; positional_limit_down_m 0.80 since
// the evening of 2026-10-08): his skinned parts' object transform sinks under his origin with it
// (measured past 0.30 m below it at a 0.60 m shift and past 0.70 m at 0.80, 1-2 cm aside), so
// his draws were taken for someone else's and drawn whole in hands_only (a coat tail by the left
// hand, ~80 draws a frame). An upright root straight under him, within DuckRootSideM aside and
// DuckRootDownM below (the deepest the floor guard lets the camera go at a 1.6 m eye, 1.25 m), is
// his: another character's capsule cannot stand inside his legs.
inline constexpr float DuckRootSideM = 0.35f;
inline constexpr float DuckRootDownM = 1.25f;
constexpr bool IsPlayersObject(float rx, float ry, float rz, bool upright) {
    const float d2 = rx * rx + ry * ry + rz * rz;
    if (d2 <= 0.30f * 0.30f) {
        return true;
    }
    if (upright && rx * rx + rz * rz <= DuckRootSideM * DuckRootSideM && ry <= 0.0f &&
        ry >= -DuckRootDownM) {
        return true;
    }
    return !upright && d2 <= 2.5f * 2.5f;
}

// body_mode=arms_only / hands_only (the renderer shows only the hunter's hands and forearms / his
// hands): what a draw of one of his face sets with a VrHandsTable key does in a pass. `on`: this
// frame is a first-person frame in such a mode; `colour`: HeadHiddenPass (the camera's G-buffer:
// the shadow maps keep his whole body); `ranges`: the key's ranges the mode keeps (0 = none of its
// triangles); `all_kept`: they are all of it; `triangles`: a triangle list (the ranges are
// triangles of a list).
enum class HandsAction : u8 {
    Untouched, // not handled here (the cape / head / cutaway rules as before)
    Skip,      // nothing of it is drawn in this pass
    Hands,     // only its hand ranges are drawn (VrDrawCapeSplit), with the draw's own pipeline
};

constexpr HandsAction PlanHands(bool on, bool colour, u32 ranges, bool all_kept,
                                bool triangles) {
    if (!on || !colour || all_kept) {
        return HandsAction::Untouched;
    }
    if (ranges == 0) {
        return HandsAction::Skip;
    }
    // Ranges need a triangle list; otherwise drawn whole (the hands must never vanish).
    return triangles ? HandsAction::Hands : HandsAction::Untouched;
}

} // namespace Vulkan::VrBodySplit
