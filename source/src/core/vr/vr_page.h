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
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/vr/graphics_settings.h"

#include "core/vr/thumb_layout.h"

// The VR settings page (Options > VR, vr.ini vr_menu=1): its tabs, rows and navigation, header-only
// and pure (unit tested in tests/test_vr_page.cpp). The values live behind a Store: the emulator's
// store (vr_page_store.cpp) reads the live settings structs, applies a change at once (the same
// parser as vr.ini) and queues the file write; the tests use a map.
//
// Layout: the game's Environment settings page (two groups of 2 + 3 rows, a button row), one tab
// per page, the grips switch tabs (right: next, left: previous; the game's own menus keep L1 / R1
// on the triggers). Navigation as in the game's own settings pages: up / down picks a row
// (wrapping, the button row last), left / right changes the value (or picks a button), and so do
// the triggers (right: next value, left: previous, as the stick pushed right / left), A confirms
// (a button), B goes back.
namespace VR::Page {

enum class Widget : unsigned char {
    ReadOnly,
    Action, // lamp action: full-width label, no setting/value controls
    Choice, // ◄ value ►, one of `options`
    Slider, // the game's slider and number box, min..max
    Number, // ◄ value ►, min..max in `step`s; 0 = `zero_text` (World size) when it has one
};

// One choice of a Choice row: what the row shows and the vr.ini value of each of its keys.
// ini_only: shown when vr.ini picks it (instead of "Custom"), but ◄ / ► never step to it (a value
// kept for experiments, e.g. Anti-aliasing's "TAA only"); stepping off it goes to the first (►) or
// last (◄) offered option, as from "Custom".
struct Option {
    const char* text;
    std::array<const char*, 2> values{nullptr, nullptr};
    bool ini_only{false};
};

struct Row {
    const char* id;    // for logs and tests
    const char* label; // the row's text (the game's voice)
    const char* help;  // the one-line help
    Widget widget{Widget::Choice};
    std::array<const char*, 2> keys{nullptr, nullptr}; // vr.ini keys (keys[1]: Choice only)
    std::vector<Option> options{};
    int min{0};
    int max{10};
    int step{1};
    const char* zero_text{nullptr}; // Number: shown for 0 (the code default: "not set")
    const char* unit{""};           // Number: after the value
    int first{0};                   // Number: the value ◄ / ► starts from when it is 0
    float ini_scale{1.0f};          // Slider: vr.ini value = slider value * ini_scale
    bool restart{false};            // applies after a restart (the help line says so)
    bool placeholder{false};        // stored, applied by a later build (the help line says so)
    // Number without a `zero_text`: a value above 0 shows its sign ("+3 cm", "0 cm", "-4 cm":
    // Adjust height).
    bool signed_value{false};
};

enum class ButtonAction : unsigned char { None, ShowControls, Defaults, AcceptGraphics, RestartGraphics };

struct Button {
    const char* label{nullptr}; // nullptr = no button there
    const char* help{""};
    ButtonAction action{ButtonAction::None};
};

inline constexpr int Slots = 5;             // rows per tab (the Environment page's)
inline constexpr int FocusLeftButton = 5;   // focus index of the left button
inline constexpr int FocusRightButton = 6;  // ... the right one

struct Tab {
    const char* name;
    std::array<const char*, 2> headers; // above slots 0 and 2
    std::array<int, Slots> rows;        // index into Rows(), -1 = empty slot
    Button left;
    Button right;
};

// ---- The rows (design.md section 3, with the design choices of 2026-10-05) ----------------------

inline const std::vector<Row>& Rows() {
    static const std::vector<Row> rows = [] {
        std::vector<Row> r;
        // General > Body (core/vr/player_body.h, the tester's design of 2026-10-09: "2 settings.
        // First option has 2 options 'As the player -- As the hunter' toggling between the two sets
        // the base height. The rest is adjustment, that goes -10+10"). Both live.
        r.push_back({"height", "Height",
                     "As the hunter: his eye height. As the player: your real height (from the "
                     "headset's floor).",
                     Widget::Choice, {"height_base", nullptr},
                     {{"As the hunter", {"hunter", nullptr}},
                      {"As the player", {"player", nullptr}}}});
        {
            // -10..+10 cm from the choice above: your eye exactly that much higher or lower.
            Row adjust{"height_adjust", "Adjust height",
                       "Taller or shorter, from the choice above. Also corrects a floor that is a "
                       "few cm off.",
                       Widget::Number, {"height_adjust_cm", nullptr}, {}, -10, 10, 1, nullptr,
                       " cm"};
            adjust.signed_value = true;
            r.push_back(adjust);
        }
        r.push_back({"stance", "Stance", "Seated: smaller swings and throws, the lap counts for the vial.",
                     Widget::Choice, {"seated", nullptr},
                     {{"Standing", {"0", nullptr}}, {"Seated", {"1", nullptr}}}});
        // Live (core/vr/thumb_layout.h): the sticks, their clicks and the face buttons trade hands.
        r.push_back({"move_with", "Move with",
                     "Which thumb walks. Your right hand still holds the blade.", Widget::Choice,
                     {"swap_sticks", nullptr},
                     {{"Left stick", {"0", nullptr}}, {"Right stick", {"1", nullptr}}}});
        // {entry:turn}: the control the scheme table binds to turning (RowLabel / RowHelp).
        r.push_back({"turning", "Turning", "How the {entry:turn} turns you.", Widget::Choice,
                     {"snap_turn_deg", "smooth_turn_deg_per_s"},
                     {{"Snap 15°", {"15", nullptr}},
                      {"Snap 30°", {"30", nullptr}},
                      {"Snap 45°", {"45", nullptr}},
                      {"Snap 90°", {"90", nullptr}},
                      {"Smooth, slow", {"0", "90"}},
                      {"Smooth", {"0", "120"}},
                      {"Smooth, fast", {"0", "180"}}}});
        r.push_back({"vibration", "Vibration", "Strength of every vibration. 0 turns it off.",
                     Widget::Slider, {"haptic_gain", nullptr}, {}, 0, 10, 1, nullptr, "", 0,
                     1.0f / 7.0f});
        r.push_back({"lamp_travel",
                     "Lamp rest & travel",
                     "Rest, refill and travel between lit lamps. Offline only. Experimental.",
                     Widget::Choice,
                     {"lamp_travel", nullptr},
                     {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        r.push_back({"physical_summon_bell", "Physical summon bell",
                     "First person: B draws the bell; shake to summon. B again cancels.",
                     Widget::Choice, {"physical_summon_bell", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"physical_levers", "Physical levers",
                     "Grip a lever with either hand and pull. Off: original interaction.",
                     Widget::Choice, {"physical_levers", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        // Controls
        r.push_back({"controls", "Control style",
                     "Motion: swing to attack. Buttons: the classic layout.", Widget::Choice,
                     {"controls", nullptr},
                     {{"Motion", {"motion", nullptr}}, {"Buttons", {"classic", nullptr}}}});
        // Stab switched on here when the game started with Button: the stab works at once with
        // the game's own drinking (the instant injection is installed at the start), hence the
        // restart. 870 of the frame's 916 stage px (the earlier 993-px line was drawn at 91%).
        r.push_back({"inject", "Blood Vial",
                     "Stab the vial into your wrist, or use a button. Restart after picking Stab.",
                     Widget::Choice, {"inject", nullptr},
                     {{"Stab to inject", {"1", nullptr}}, {"Button", {"0", nullptr}}}});
        r.push_back({"laser", "Gun laser", "A thin line from the gun's barrel.", Widget::Choice,
                     {"laser", nullptr}, {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        // {entry:grip_adjust}: the controls the scheme table binds to it (RowLabel).
        r.push_back({"grip_adjust", "Grip adjust ({entry:grip_adjust})",
                     "Press {entry:grip_adjust} twice and hold to set how the weapon sits in your "
                     "hand.",
                     Widget::Choice, {"grip_adjust", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"vr_grab_attacks", "VR Grab attacks",
                     "Stay in VR during enemy grabs. The view follows the hunter. Off: existing behavior.",
                     Widget::Choice, {"vr_grab_attacks", nullptr},
                     {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        // Comfort
        // Scene presentation remains configurable in vr.ini, not on the settings page.
        r.push_back({"smooth_motion", "Smooth motion",
                     "Walking and turning move on between the game's frames.", Widget::Choice,
                     {"late_warp_motion", nullptr}, {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"cutaway", "Body near your eyes",
                     "Full body only: fade his coat where it comes close to your eyes.",
                     Widget::Choice, {"body_cutaway", nullptr},
                     {{"Fade", {"1", nullptr}}, {"Show", {"0", nullptr}}}});
        r.push_back({"body_mode", "Hunter's body",
                     "See the hunter's whole body, only his hands and forearms, or only his hands.",
                     Widget::Choice, {"body_mode", nullptr},
                     {{"Full", {"no_head", nullptr}},
                      {"Arms only", {"arms_only", nullptr}},
                      {"Hands only", {"hands_only", nullptr}}}});
        r.push_back({"ipd", "World size (eye distance)",
                     "A smaller eye distance makes the world look larger.", Widget::Number,
                     {"ipd_mm", nullptr}, {}, 40, 80, 1, "Headset", " mm", 63});
        // HUD
        r.push_back({"hud_vitals", "Health & stamina",
                     "On your wrist, or in the corner like the game.", Widget::Choice,
                     {"hud_vitals", nullptr},
                     {{"Wrist gauge", {"wrist", nullptr}}, {"Classic", {"classic", nullptr}}}});
        r.push_back({"low_hp", "Low-health warning",
                     "A heartbeat in both hands and dark edges when your health is low.",
                     Widget::Choice, {"vitals_heartbeat", "vitals_edge"},
                     {{"On", {"1", "1"}}, {"Off", {"0", "0"}}}});
        r.push_back({"counters", "Echoes & Insight",
                     "Look down: they show when you look down at your feet.", Widget::Choice,
                     {"ui_counters", nullptr},
                     {{"Look down", {"look_down", nullptr}},
                      {"Always", {"always", nullptr}},
                      {"Hidden", {"hidden", nullptr}}}});
        r.push_back({"hit_flash", "Hit flash", "The edges of your view flash red when you are hit.",
                     Widget::Choice, {"hit_vignette", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"hud_size", "HUD size", "The size of the health bars, items and prompts.",
                     Widget::Choice, {"ui_hud_scale_deg", nullptr},
                     {{"Small", {"0.035", nullptr}},
                      {"Medium", {"0.042", nullptr}},
                      {"Large", {"0.05", nullptr}}}});
        // Assists
        r.push_back({"attack_stamina", "Light attack stamina",
                     "Light swing cost. Strong swings cost 100%; low stamina makes them light.",
                     Widget::Choice, {"melee_stamina_pct", nullptr},
                     {{"Original", {"100", nullptr}}, {"80%", {"80", nullptr}},
                      {"50%", {"50", nullptr}}, {"10%", {"10", nullptr}}}});
        r.push_back({"outgoing_damage", "Damage dealt",
                     "Your attack damage: -30% to +30%. 0% is normal. Applies immediately.",
                     Widget::Number, {"outgoing_damage_pct", nullptr}, {},
                     -30, 30, 5, nullptr, "%"});
        r.back().signed_value = true;
        r.push_back({"hp_bonus", "Extra health",
                     "More maximum health for the hunter. Applies while playing.",
                     Widget::Choice, {"player_hp_bonus_pct", nullptr},
                     {{"Off", {"0", nullptr}},
                      {"+10 %", {"10", nullptr}},
                      {"+20 %", {"20", nullptr}},
                      {"+30 %", {"30", nullptr}},
                      {"+50 %", {"50", nullptr}}},
                     0, 10, 1, nullptr, "", 0, 1.0f, false});
        r.push_back({"iframes", "Longer dodge invincibility",
                     "Extra frames of invincibility in a dodge. Changes apply to your next dodge.",
                     Widget::Choice, {"dodge_iframes_extra", nullptr},
                     {{"Off", {"0", nullptr}},
                      {"+1 frame", {"1", nullptr}},
                      {"+2 frames", {"2", nullptr}},
                      {"+3 frames", {"3", nullptr}},
                      {"+4 frames", {"4", nullptr}}},
                     0, 10, 1, nullptr, "", 0, 1.0f, false});
        // Blade kills (core/vr/bloodborne_ragdoll.h, 2026-10-08): the instant ragdoll on a
        // right-hand melee kill and its push, live: the damage detour reads melee_kill_ragdoll at
        // every hit and the push reads melee_ragdoll_impulse when it is applied. On, a kill still
        // keeps the game's own death with vr.ini melee_ragdoll_vanilla_pct's chance (22%; no row).
        r.push_back({"kill_ragdoll", "Instant ragdoll",
                     // 70 characters (69 measured 844 of the line's 900 stage px).
                     "An enemy your blade kills goes limp at once. Off: its death animation.",
                     Widget::Choice, {"melee_kill_ragdoll", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"ragdoll_push", "Ragdoll push",
                     "How hard your swing throws the body. None: it just falls.", Widget::Choice,
                     {"melee_ragdoll_impulse", nullptr},
                     {{"None", {"0", nullptr}},
                      {"Light", {"0.5", nullptr}},
                      {"Normal", {"1", nullptr}},
                      {"Strong", {"2", nullptr}},
                      {"Strongest", {"3", nullptr}}}});
        for (const auto& row : std::array<Row, 3>{{
                 {"color_contrast", "Contrast", "Light and dark separation. 0 is unchanged.",
                  Widget::Number, {"color_contrast"}, {}, -50, 50, 5, nullptr, "%"},
                 {"color_saturation", "Saturation", "Color intensity. -100 is monochrome; 0 is unchanged.",
                  Widget::Number, {"color_saturation"}, {}, -100, 100, 5, nullptr, "%"},
                 {"color_gamma", "Gamma", "Midtone brightness. Higher is brighter; 0 is unchanged.",
                  Widget::Number, {"color_gamma"}, {}, -50, 50, 5, nullptr, "%"}}}) {
            r.push_back(row);
            r.back().signed_value = true;
        }
        // Graphics (core/vr/aa_mode.h): the game frame's anti-aliasing, live from the next frame.
        // Spatial AA preference is preserved while an accepted upscaler bypasses it.
        r.push_back({"aa", "Anti-aliasing",
                     // About 870 of the frame's 900 stage px at 24 px (the menu font; a line of
                     // 69 characters measured 844). A wider line shrinks to fit.
                     "Game FXAA, dual-eye SMAA, or None. Bypassed while DLSS or FSR is on.",
                     Widget::Choice, {"aa_mode", nullptr},
                     {{"Game FXAA", {"fxaa", nullptr}},
                      {"SMAA", {"smaa", nullptr}},
                      {"None", {"off", nullptr}}}});
        // Graphics > Display (vr.ini lod_bias, 2026-10-17): the Model LOD patch's bias, read as
        // the game starts.
        r.push_back({"lod", "Distant detail",
                     "Performance: the game's simpler distant models, fastest. {lod_apply}",
                     Widget::Choice, {"lod_bias", nullptr},
                     {{"Performance", {"0", nullptr}},
                      {"Balanced", {"-1", nullptr}},
                      {"Quality", {"-2", nullptr}}}});
        // Graphics > Display (core/vr/bloodborne_enemy_bars.h, 2026-10-08): the bars over the
        // enemies you hit or lock on to, with their damage numbers, live (the EnemyTag visibility
        // hook reads ui_enemy_hp_bars at every tag update). The HUD tab's five slots are taken.
        r.push_back({"enemy_bars", "Show enemy health bars",
                     "The bars over enemies you hit or lock on to.",
                     Widget::Choice, {"ui_enemy_hp_bars", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        // Graphics > Display (2026-10-15): the boss's bar, shown by default (2026-10-18); shown,
        // it moves near the top of the view (bloodborne_enemy_bars.h), live.
        r.push_back({"boss_bar", "Show boss health bar",
                     "The boss's bar, shown near the top of the view.",
                     Widget::Choice, {"ui_boss_bar", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"graphics_resolution",
                     "Headset resolution",
                     "Recommended per-eye resolution reported by OpenXR.",
                     Widget::ReadOnly,
                     {"graphics_resolution"}});
        Row scale{"graphics_scale",
                  "Scaling",
                  "Relative to headset resolution. ACCEPT saves it; restart the game to resize scene and output.",
                  Widget::Number,
                  {"graphics_scale"},
                  {},
                  -50,
                  50,
                  10,
                  nullptr,
                  "%"};
        scale.signed_value = true;
        r.push_back(scale);
        r.push_back({"graphics_dlss",
                     "DLSS",
                     "Render smaller and reconstruct both eyes. DLSS turns FSR and DLSS 5 off.",
                     Widget::Choice,
                     {"graphics_dlss"},
                     {{"Off", {"0"}}, {"On", {"1"}}}});
        r.push_back({"graphics_fsr4",
                     "FSR",
                     "Choose FSR 4 or FSR 3.1.5 to reconstruct both eyes. Turns DLSS off.",
                     Widget::Choice,
                     {"graphics_fsr4"},
                     {{"Off", {"0"}}, {"FSR 4", {"1"}}, {"FSR 3.1.5", {"2"}}}});
        const std::vector<Option> profiles{
            {"Performance", {"0"}}, {"Balanced", {"1"}}, {"Quality", {"2"}}};
        r.push_back({"graphics_mode",
                     "Profile",
                     "Requires DLSS or FSR. DLSS and FSR remember their profiles.",
                     Widget::Choice,
                     {"graphics_mode"},
                     profiles});
        r.push_back({"graphics_neural",
                     "DLSS 5",
                     "Neural lighting and detail. Turns regular DLSS and FSR off; ACCEPT applies.",
                     Widget::Choice,
                     {"graphics_neural"},
                     {{"Off", {"0"}}, {"On", {"1"}}}});
        r.push_back({"graphics_neural_mode",
                     "Profile",
                     "Controls render and neural resolution together. Balanced is the default.",
                     Widget::Choice,
                     {"graphics_neural_mode"},
                     {{"Performance", {"0"}}, {"Balanced", {"1"}}}});
        r.push_back({"graphics_intensity",
                     "Enhancement",
                     "Strength of the neural treatment. Zero keeps the original look.",
                     Widget::Number,
                     {"graphics_intensity"},
                     {},
                     0,
                     100,
                     5,
                     nullptr,
                     "%"});
        r.push_back({"graphics_detail",
                     "Preserve detail",
                     "Preserve the structure of the hunter, weapons and architecture.",
                     Widget::Number,
                     {"graphics_detail"},
                     {},
                     0,
                     100,
                     5,
                     nullptr,
                     "%"});
        r.push_back({"graphics_lighting",
                     "Lighting treatment",
                     "Strength of the neural lighting treatment. Requires DLSS 5.",
                     Widget::Number,
                     {"graphics_lighting"},
                     {},
                     0,
                     100,
                     5,
                     nullptr,
                     "%"});
        r.push_back(
            {"body_follow",
             "Tracked posture",
             "Lean keeps the spine's length as you move your head.",
             Widget::Choice,
             {"body_align", "body_posture"},
             {{"Lean", {"1", "lean"}}, {"Legacy shift", {"1", "shift"}}, {"Off", {"0", "lean"}}}});
        r.push_back({"lean_side",
                     "Side lean limit",
                     "How far the hunter leans sideways. More lean lets you move farther "
                     "before he steps.",
                     Widget::Slider,
                     {"body_lean_side_deg", nullptr},
                     {},
                     0,
                     35,
                     1,
                     nullptr,
                     " deg"});
        r.push_back({"lean_response",
                     "Posture smoothing",
                     "Less smoothing follows faster; more softens small movements.",
                     Widget::Slider,
                     {"body_align_ms", nullptr},
                     {},
                     0,
                     500,
                     25,
                     nullptr,
                     " ms"});
        r.push_back({"body_comfort",
                     "Extra clipping protection",
                     "Protect sideways leans and restore the body in stages after a roll or hit.",
                     Widget::Choice,
                     {"body_comfort", nullptr},
                     {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        r.push_back(
            {"holster_mode",
             "Item access",
             "Left grip takes and holds items. Release returns an unused vial, or throws a "
             "throwable. Vials stay right; pocket/chest follow equipped quick slots 1/2.",
             Widget::Choice,
             {"holster_mode", nullptr},
             {{"Menu", {"0", nullptr}}, {"Body slots", {"1", nullptr}}, {"Both", {"2", nullptr}}}});
        r.push_back({"holster_haptics",
                     "Pouch touch feedback",
                     "A small vibration when your hand finds a pouch.",
                     Widget::Choice,
                     {"holster_haptics", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"shoulder_switch",
                     "Shoulder weapon switch",
                     "B at right shoulder / Y at left: tap to switch, hold about 0.7 seconds for unarmed. Tap unarmed to restore the remembered weapon.",
                     Widget::Choice,
                     {"shoulder_switch", nullptr},
                     {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        r.push_back(
            {"health_color",
             "Low-health red edges",
             "A restrained red edge below 30% health. Takes priority over stamina color feedback.",
             Widget::Choice,
             {"health_color", nullptr},
             {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        r.push_back({"health_color_strength",
                     "Health intensity",
                     "Strength of the low-health red edge.",
                     Widget::Slider,
                     {"health_color_strength", nullptr},
                     {},
                     0,
                     100,
                     10,
                     nullptr,
                     "%",
                     0,
                     .01f});
        r.push_back(
            {"stamina_color",
             "Exhaustion desaturation",
             "Briefly soften colors at exhausted stamina, then fade back. Never full grayscale.",
             Widget::Choice,
             {"stamina_color", nullptr},
             {{"Off", {"0", nullptr}}, {"On", {"1", nullptr}}}});
        r.push_back({"stamina_color_strength",
                     "Stamina intensity",
                     "Strength of the brief exhaustion effect.",
                     Widget::Slider,
                     {"stamina_color_strength", nullptr},
                     {},
                     0,
                     100,
                     10,
                     nullptr,
                     "%",
                     0,
                     .01f});
        r.push_back({"gun_wrist_reload", "Flick to reload",
                     "Wrist flick: release the trigger and flick firmly after each shot. Off/Original: fire again without a flick.",
                     Widget::Choice, {"gun_wrist_reload", nullptr},
                     {{"Wrist flick", {"1", nullptr}}, {"Off/Original", {"0", nullptr}}}});
        r.push_back({"weighted_weapons", "Weighted weapons",
                     "Weight, gravity and air resistance for rigid weapons. Light softens these effects. Whip forms are excluded.",
                     Widget::Choice, {"weighted_weapons", nullptr},
                     {{"Off", {"0", nullptr}}, {"On (Light)", {"2", nullptr}},
                      {"On (Regular)", {"1", nullptr}}}}); // regular by default
        // vr.ini weapon_wall: defaults to 2, weapons solid against each other but
        // passing through walls. 0 disables solid weapons; 1 also enables walls.
        r.push_back({"weapon_wall", "Solid weapons",
                     "Stop at walls and each other. Weapons only: just each other.",
                     Widget::Choice, {"weapon_wall", nullptr},
                     {{"Off", {"0", nullptr}},
                      {"On", {"1", nullptr}},
                      {"Weapons only", {"2", nullptr}}}});
        // vr.ini blade_sounds (core/vr/blade_sounds.h; 2026-10-13).
        r.push_back({"blade_sounds", "Blade sounds",
                     "Your weapons ring when they meet; draw one along the other to sharpen it.",
                     Widget::Choice, {"blade_sounds", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"two_hand_guide", "Grip guide",
                     "Hold the right grip to show where your left hand can grab.",
                     Widget::Choice, {"two_hand_guide", nullptr},
                     {{"On", {"1", nullptr}}, {"Off", {"0", nullptr}}}});
        r.push_back({"two_hand_swing_hum", "Swing feel",
                     "Faint feedback during fast swings with both hands on one weapon.",
                     Widget::Slider, {"two_hand_swing_hum", nullptr}, {},
                     0, 100, 10, nullptr, "%", 0, .01f});
        return r;
    }();
    return rows;
}

inline int RowIndex(const char* id) {
    const auto& rows = Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (std::string{rows[i].id} == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline const std::vector<Tab>& Tabs() {
    static const std::vector<Tab> tabs = [] {
        const Button controls{"Show VR controls", "See what each control does.",
                              ButtonAction::ShowControls};
        const Button defaults{"Defaults", "Return this page to its defaults.",
                              ButtonAction::Defaults};
        std::vector<Tab> t;
        // Body: Height and Adjust height (2026-10-09); Stance is on Controls (it sizes the swings,
        // throws and the vial's lap).
        t.push_back({"General",
                     {"Body", "Controllers"},
                     {RowIndex("height"), RowIndex("height_adjust"), RowIndex("move_with"),
                      RowIndex("turning"), RowIndex("vibration")},
                     controls,
                     defaults});
        t.push_back({"Controls",
                     {"Style", "Aids"},
                     {RowIndex("controls"), RowIndex("inject"), RowIndex("laser"),
                      RowIndex("grip_adjust"), RowIndex("stance")},
                     controls,
                     defaults});
        t.push_back({"Comfort",
                     {"View", "Body"},
                     {RowIndex("smooth_motion"), -1, RowIndex("cutaway"),
                      RowIndex("body_mode"), RowIndex("ipd")},
                     {},
                     defaults});
        t.push_back({"HUD",
                     {"Vitals", "Display"},
                     {RowIndex("hud_vitals"), RowIndex("low_hp"), RowIndex("counters"),
                      RowIndex("hit_flash"), RowIndex("hud_size")},
                     {},
                     defaults});
        t.push_back({"Assists",
                     {"Combat", "Attacks"},
                     {RowIndex("hp_bonus"), RowIndex("iframes"), RowIndex("kill_ragdoll"),
                      RowIndex("ragdoll_push"), RowIndex("attack_stamina")},
                     {},
                     defaults});
        // Existing display controls keep their immediate-apply behavior.
        t.push_back({"Display",
                     {"Image", "Display"},
                     {RowIndex("aa"), RowIndex("lod"), RowIndex("enemy_bars"), RowIndex("boss_bar"),
                      -1},
                     {},
                     defaults});
        const Button accept{"ACCEPT", "Save all pending Graphics and DLSS 5 choices together.",
                            ButtonAction::AcceptGraphics};
        const Button restart{"SAVE AND RESTART", "Save graphics settings and restart. Progress since the last autosave may be lost.",
                             ButtonAction::RestartGraphics};
        t.push_back(
            {"Graphics",
             {"Headset", "Upscaling"},
             {RowIndex("graphics_resolution"), RowIndex("graphics_scale"),
              RowIndex("graphics_dlss"), RowIndex("graphics_fsr4"), RowIndex("graphics_mode")},
             restart,
             accept});
        t.push_back({"DLSS 5",
                     {"Neural rendering", "Appearance"},
                     {RowIndex("graphics_neural"), RowIndex("graphics_neural_mode"),
                      RowIndex("graphics_intensity"), RowIndex("graphics_detail"),
                      RowIndex("graphics_lighting")},
                     restart,
                     accept});
        t.push_back({"Body",
                     {"Posture", "Protection"},
                     {RowIndex("body_follow"), RowIndex("lean_side"), RowIndex("body_comfort"),
                      RowIndex("lean_response"), -1},
                     {},
                     defaults});
        t.push_back(
            {"Inventory",
             {"Access", nullptr},
             {RowIndex("holster_mode"), RowIndex("holster_haptics"), -1, -1, -1},
             {},
             defaults});
        t.push_back({"Feedback",
                     {"Health", "Stamina"},
                     {RowIndex("health_color"), RowIndex("health_color_strength"),
                      RowIndex("stamina_color"), RowIndex("stamina_color_strength"),
                      RowIndex("shoulder_switch")},
                     {},
                     defaults});
        t.push_back({"Weapons", {"Handling", nullptr},
                     {RowIndex("weapon_wall"), RowIndex("two_hand_guide"),
                      RowIndex("two_hand_swing_hum"), RowIndex("gun_wrist_reload"),
                      RowIndex("blade_sounds")}, {},
                     defaults});
        t.push_back({"Interactions",
                     {"Hands", "Enemy attacks"},
                     {RowIndex("physical_levers"), RowIndex("lamp_travel"),
                      RowIndex("vr_grab_attacks"), RowIndex("physical_summon_bell"), -1},
                     {},
                     defaults});
        t.push_back({"Color", {"Color", "Brightness"},
                     {RowIndex("color_contrast"), RowIndex("color_saturation"),
                      RowIndex("color_gamma"), -1, -1}, {}, defaults});
        t.push_back({"Weapon weight", {"Handling", nullptr},
                     {RowIndex("weighted_weapons"), -1, -1, -1, -1}, {}, defaults});
        t.push_back({"Damage", {"Attacks", nullptr},
                     {RowIndex("outgoing_damage"), -1, -1, -1, -1}, {}, defaults});
        return t;
    }();
    return tabs;
}

// ---- Values -------------------------------------------------------------------------------------

// Where the values live. The emulator's store applies a Set at once (live) and saves it; Remove
// returns the key to its code default (live) and takes it out of vr.ini.
class Store {
public:
    virtual ~Store() = default;
    virtual std::string ControlName(Thumbs::Control physical) {
        return Thumbs::Label(physical);
    }
    virtual std::string Get(const std::string& key) = 0;
    virtual void Set(const std::string& key, const std::string& value) = 0;
    virtual void Remove(const std::string& key) = 0;
};

// Only graphics edits are staged. Existing gameplay/Display settings retain their behavior.
class GraphicsDraftStore final : public Store {
    Store& source;
    std::optional<Graphics::Config> pending;
    Graphics::Config Current() {
        if (pending)
            return *pending;
        return Graphics::Parse(source.Get("graphics_config")).value_or(Graphics::Config{});
    }

public:
    explicit GraphicsDraftStore(Store& s) : source(s) {}
    std::string ControlName(Thumbs::Control physical) override {
        return source.ControlName(physical);
    }
    bool Pending() const {
        return pending && *pending != Graphics::Parse(source.Get("graphics_config")).value_or(Graphics::Config{});
    }
    void Discard() {
        pending.reset();
    }
    bool Accept() {
        if (!Pending()) {
            pending.reset();
            return false;
        }
        source.Set("graphics_config", Graphics::Serialize(*pending));
        pending.reset();
        return true;
    }
    std::string Get(const std::string& key) override {
        if (!key.starts_with("graphics_") || key == "graphics_resolution" ||
            key == "graphics_config" || key == "graphics_status" ||
            key == "graphics_fsr4_available" || key == "graphics_fsr31_available" || key == "graphics_neural_available" ||
            key == "graphics_native_stereo")
            return source.Get(key);
        const auto c = Current();
        if (key == "graphics_scale")
            return std::to_string(c.scale_percent);
        if (key == "graphics_dlss")
            return c.upscaler == Graphics::Upscaler::Dlss && !c.dlss5 ? "1" : "0";
        if (key == "graphics_fsr4")
            return c.upscaler == Graphics::Upscaler::Fsr4 ? "1" :
                   c.upscaler == Graphics::Upscaler::Fsr31 ? "2" : "0";
        if (key == "graphics_fsr31")
            return c.upscaler == Graphics::Upscaler::Fsr31 ? "1" : "0";
        if (key == "graphics_mode")
            return std::to_string(int(Graphics::IsFsr(c.upscaler) ? c.fsr4 : c.dlss));
        if (key == "graphics_neural")
            return c.upscaler == Graphics::Upscaler::Dlss && c.dlss5 ? "1" : "0";
        if (key == "graphics_neural_mode")
            return std::to_string(int(c.neural));
        if (key == "graphics_intensity")
            return std::to_string(c.intensity);
        if (key == "graphics_detail")
            return std::to_string(c.detail);
        if (key == "graphics_lighting")
            return std::to_string(c.lighting);
        return {};
    }
    void Set(const std::string& key, const std::string& value) override {
        if (!key.starts_with("graphics_")) {
            source.Set(key, value);
            return;
        }
        auto c = Current();
        const int n = std::atoi(value.c_str());
        if (key == "graphics_scale")
            c.scale_percent = std::clamp(n, -50, 50) / 10 * 10;
        else if (key == "graphics_dlss") {
            if (n) {
                c.upscaler = Graphics::Upscaler::Dlss;
                c.dlss5 = false;
            } else if (c.upscaler == Graphics::Upscaler::Dlss && !c.dlss5)
                c.upscaler = Graphics::Upscaler::Off;
        } else if (key == "graphics_fsr4") {
            if ((n == 1 && source.Get("graphics_fsr4_available") == "0") ||
                (n == 2 && source.Get("graphics_fsr31_available") == "0")) return;
            if (n) {
                c.upscaler = n == 2 ? Graphics::Upscaler::Fsr31 : Graphics::Upscaler::Fsr4;
                c.dlss5 = false;
            } else if (Graphics::IsFsr(c.upscaler))
                c.upscaler = Graphics::Upscaler::Off;
        } else if (key == "graphics_fsr31") {
            if (n) {
                c.upscaler = Graphics::Upscaler::Fsr31;
                c.dlss5 = false;
            } else if (c.upscaler == Graphics::Upscaler::Fsr31)
                c.upscaler = Graphics::Upscaler::Off;
        } else if (key == "graphics_mode") {
            if (c.upscaler == Graphics::Upscaler::Off || c.dlss5)
                return;
            (c.upscaler == Graphics::Upscaler::Dlss ? c.dlss : c.fsr4) =
                Graphics::Profile(std::clamp(n, 0, 2));
        } else if (key == "graphics_neural") {
            // DLSS 5 owns its reconstruction internally; the three menu choices are exclusive.
            if (n) c.upscaler = Graphics::Upscaler::Dlss;
            else if (c.dlss5 && c.upscaler == Graphics::Upscaler::Dlss)
                c.upscaler = Graphics::Upscaler::Off;
            c.dlss5 = n != 0;
        } else if (key == "graphics_neural_mode")
            Graphics::SetNeuralProfile(c, Graphics::Profile(std::clamp(n, 0, 2)));
        else if (key == "graphics_intensity")
            c.intensity = std::clamp(n, 0, 100);
        else if (key == "graphics_detail")
            c.detail = std::clamp(n, 0, 100);
        else if (key == "graphics_lighting")
            c.lighting = std::clamp(n, 0, 100);
        else
            return;
        pending = c;
    }
    void Remove(const std::string& key) override {
        if (!key.starts_with("graphics_")) {
            source.Remove(key);
            return;
        }
        if (key == "graphics_resolution")
            return;
        if (key == "graphics_mode") {
            auto c = Current();
            c.dlss = c.fsr4 = Graphics::Profile::Balanced;
            pending = c;
        } else if (key == "graphics_neural_mode")
            Set(key, "1");
        else if (key == "graphics_intensity")
            Set(key, "65");
        else if (key == "graphics_detail")
            Set(key, "85");
        else if (key == "graphics_lighting")
            Set(key, "60");
        else
            Set(key, "0");
    }
};
inline bool Enabled(const Row& row, Store& store) {
    const std::string_view id = row.id;
    if (row.widget == Widget::ReadOnly)
        return false;
    // The current reconstruction path needs native stereo's paired images and motion.
    // Preserve the saved preference, but keep DIBR's working spatial AA accessible.
    if (store.Get("graphics_native_stereo") == "0") {
        if (id == "aa" || id == "graphics_scale") return true;
        if (id.starts_with("graphics_")) return false;
    }
    if (id == "aa") {
        const auto accepted = Graphics::Parse(store.Get("graphics_config"));
        return !accepted || accepted->upscaler == Graphics::Upscaler::Off;
    }
    // A release without FSR 4 or DLSS 5 (their files absent): those choices are greyed. A store
    // that does not know (empty) leaves them on.
    if (id == "graphics_fsr31")
        return store.Get("graphics_fsr31_available") != "0";
    if (id == "graphics_fsr4")
        return store.Get("graphics_fsr4_available") != "0" || store.Get("graphics_fsr31_available") == "1";
    if (id == "graphics_neural")
        return store.Get("graphics_neural_available") != "0";
    if (id == "graphics_mode")
        return store.Get("graphics_dlss") == "1" || store.Get("graphics_fsr4") == "1" || store.Get("graphics_fsr31") == "1";
    if (id == "graphics_neural_mode" || id == "graphics_intensity" || id == "graphics_detail" ||
        id == "graphics_lighting")
        return store.Get("graphics_neural") == "1";
    return true;
}

inline bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

inline bool SameValue(const std::string& a, const char* b) {
    if (b == nullptr) {
        return true;
    }
    double x{}, y{};
    if (ParseNumber(a, x) && ParseNumber(b, y)) {
        return std::abs(x - y) < 1e-4 * std::max(1.0, std::abs(y));
    }
    if ((a == "true" && std::string{b} == "1") || (a == "false" && std::string{b} == "0")) {
        return true;
    }
    return a == b;
}

// Move with the right stick (swap_sticks) as the store has it now.
inline bool SticksSwapped(Store& store) {
    const std::string v = store.Get("swap_sticks");
    return v == "1" || v == "true";
}

// The layout the store has now (left_scheme, the control style, Move with, the vial), for the
// scheme table (core/vr/thumb_layout.h Entries).
inline VR::Thumbs::Layout PageLayout(Store& store) {
    VR::Thumbs::Layout l{};
    double scheme{};
    l.left_scheme = ParseNumber(store.Get("left_scheme"), scheme) ? static_cast<int>(scheme) : 2;
    l.classic = store.Get("controls") == "classic";
    l.swap = SticksSwapped(store);
    const std::string inject = store.Get("inject");
    l.inject = inject != "0" && inject != "false";
    return l;
}

// "{entry:<id>}": the controls the scheme table binds to that entry in the current layout ("A+B"
// for a chord, "hold X + Y" for a combo), so a row names what the table says.
inline std::string ExpandEntries(std::string text, Store& store) {
    static constexpr std::string_view Open = "{entry:";
    size_t at = text.find(Open);
    if (at == std::string::npos) {
        return text;
    }
    const VR::Thumbs::Layout layout = PageLayout(store);
    while (at != std::string::npos) {
        const size_t close = text.find('}', at);
        if (close == std::string::npos) {
            break;
        }
        const std::string id = text.substr(at + Open.size(), close - at - Open.size());
        const VR::Thumbs::Bound b = VR::Thumbs::Find(layout, id);
        std::string what = "?";
        if (b.first != VR::Thumbs::Control::None) {
            what = store.ControlName(b.first);
            if (b.second != VR::Thumbs::Control::None) {
                const bool chord = b.entry.how == VR::Thumbs::How::Chord;
                what = std::string{chord ? "" : "hold "} + what + (chord ? "+" : " + ") +
                       store.ControlName(b.second);
            }
        }
        text.replace(at, close + 1 - at, what);
        at = text.find(Open, at + what.size());
    }
    return text;
}

// A row's label and help line with the controls of the current layout: "{turn}" and "{A}" name
// the stick and buttons the player presses (core/vr/thumb_layout.h Expand), "{entry:<id>}" the
// controls of a scheme-table entry, so Move with and the scheme rename them at once.
inline std::string RowLabel(const Row& row, Store& store) {
    return VR::Thumbs::Expand(ExpandEntries(row.label != nullptr ? row.label : "", store),
                              SticksSwapped(store), [&](Thumbs::Control physical) {
                                  return store.ControlName(physical);
                              });
}
inline std::string RowHelp(const Row& row, Store& store) {
    std::string help = row.help != nullptr ? row.help : "";
    // Distant detail: live when the LOD patch reads its bias from a slot (memory_patcher), else
    // saved for the next start (store key lod_bias_live: "0" only when it cannot be live).
    if (const auto at = help.find("{lod_apply}"); at != std::string::npos) {
        help.replace(at, 11, store.Get("lod_bias_live") == "0" ? "Applies at the next start."
                                                                : "Applies at once.");
    }
    return VR::Thumbs::Expand(ExpandEntries(help, store),
                              SticksSwapped(store), [&](Thumbs::Control physical) {
                                  return store.ControlName(physical);
                              });
}

// A Choice row's current option (-1: a value the page does not offer, "Custom").
inline int CurrentOption(const Row& row, Store& store) {
    std::array<std::string, 2> now{};
    for (int k = 0; k < 2; ++k) {
        if (row.keys[k] != nullptr) {
            now[k] = store.Get(row.keys[k]);
        }
    }
    // Turning: a snap angle above 0 is a snap option whatever the smooth speed.
    for (size_t i = 0; i < row.options.size(); ++i) {
        const Option& o = row.options[i];
        bool match = SameValue(now[0], o.values[0]);
        if (match && row.keys[1] != nullptr && o.values[1] != nullptr) {
            match = SameValue(now[1], o.values[1]);
        }
        if (match) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline int CurrentNumber(const Row& row, Store& store) {
    double v{};
    if (!ParseNumber(store.Get(row.keys[0]), v)) {
        return 0;
    }
    if (row.widget == Widget::Slider) {
        return std::clamp(static_cast<int>(std::lround(v / row.ini_scale)), row.min, row.max);
    }
    return static_cast<int>(std::lround(v));
}

inline std::string FormatIni(double v) {
    if (std::abs(v - std::round(v)) < 1e-6) {
        return std::to_string(static_cast<long long>(std::llround(v)));
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", v);
    return buffer;
}

// A Number row's text for `n`: its `zero_text` for 0 when it has one, else the number and the unit
// ("+3 cm" with signed_value).
inline std::string NumberText(const Row& row, int n) {
    if (n == 0 && row.zero_text != nullptr) {
        return row.zero_text;
    }
    return (row.signed_value && n > 0 ? "+" : "") + std::to_string(n) + row.unit;
}

inline std::string ValueText(const Row& row, Store& store) {
    switch (row.widget) {
    case Widget::Action:
        return {};
    case Widget::ReadOnly:
        return store.Get(row.keys[0]);
    case Widget::Choice: {
        const int o = CurrentOption(row, store);
        return o >= 0 ? row.options[static_cast<size_t>(o)].text : "Custom";
    }
    case Widget::Slider:
        return std::to_string(CurrentNumber(row, store));
    case Widget::Number:
        return NumberText(row, CurrentNumber(row, store));
    }
    return {};
}

// What one change did (for the log and the tests).
struct Change {
    std::string row;
    std::string text;                                   // the new value as shown
    std::vector<std::pair<std::string, std::string>> set; // key, value
    std::vector<std::string> removed;
    bool restart{false};
    bool placeholder{false};
};

// Steps `row` by `delta` (Choice: the next / previous option, wrapping like the game's combo boxes;
// Slider / Number: by `step`, clamped; a Number with a `zero_text` starts at `first` from 0 and
// goes back to 0 below its range). Applies through the store. A Choice steps through its offered
// options only: an ini_only one is skipped.
inline Change Step(const Row& row, Store& store, int delta) {
    Change c{row.id, {}, {}, {}, row.restart, row.placeholder};
    if (delta == 0 || !Enabled(row, store)) {
        c.text = ValueText(row, store);
        return c;
    }
    switch (row.widget) {
    case Widget::Action:
    case Widget::ReadOnly:
        break;
    case Widget::Choice: {
        std::vector<int> offered;
        for (size_t i = 0; i < row.options.size(); ++i) {
            const auto& option = row.options[i];
            const bool missing_fsr = std::string_view(row.id) == "graphics_fsr4" &&
                ((SameValue("1", option.values[0]) && store.Get("graphics_fsr4_available") == "0") ||
                 (SameValue("2", option.values[0]) && store.Get("graphics_fsr31_available") == "0"));
            if (!option.ini_only && !missing_fsr) {
                offered.push_back(static_cast<int>(i));
            }
        }
        const int count = static_cast<int>(offered.size());
        if (count == 0) {
            c.text = ValueText(row, store);
            break;
        }
        const int now = CurrentOption(row, store);
        int p = -1; // the current option's place among the offered ones
        for (int i = 0; i < count; ++i) {
            if (offered[static_cast<size_t>(i)] == now) {
                p = i;
            }
        }
        p = p < 0 ? (delta > 0 ? 0 : count - 1) : ((p + delta) % count + count) % count;
        const Option& opt = row.options[static_cast<size_t>(offered[static_cast<size_t>(p)])];
        for (int k = 0; k < 2; ++k) {
            if (row.keys[k] != nullptr && opt.values[k] != nullptr) {
                store.Set(row.keys[k], opt.values[k]);
                c.set.emplace_back(row.keys[k], opt.values[k]);
            }
        }
        c.text = opt.text;
        break;
    }
    case Widget::Slider: {
        const int now = CurrentNumber(row, store);
        const int next = std::clamp(now + delta * row.step, row.min, row.max);
        const std::string v = FormatIni(next * static_cast<double>(row.ini_scale));
        store.Set(row.keys[0], v);
        c.set.emplace_back(row.keys[0], v);
        c.text = std::to_string(next);
        break;
    }
    case Widget::Number: {
        const int now = CurrentNumber(row, store);
        int next = now + delta * row.step;
        if (row.zero_text == nullptr) {
            // 0 is a value like any other (Adjust height): the range clamps.
            next = std::clamp(next, row.min, row.max);
        } else if (now == 0) {
            next = row.first; // from "Headset" (World size)
        } else if (next < row.min) {
            next = 0; // below the range: back to "Headset"
        } else if (next > row.max) {
            next = std::clamp(next, row.min, row.max);
        }
        const std::string v = std::to_string(next);
        store.Set(row.keys[0], v);
        c.set.emplace_back(row.keys[0], v);
        c.text = NumberText(row, next);
        break;
    }
    }
    return c;
}

// Slider: set by a fraction 0..1 of the bar (pointer drag).
inline Change SetFraction(const Row& row, Store& store, float fraction) {
    Change c{row.id, {}, {}, {}, row.restart, row.placeholder};
    const int value = std::clamp(
        static_cast<int>(std::lround(row.min + std::clamp(fraction, 0.0f, 1.0f) * (row.max - row.min))),
        row.min, row.max);
    if (value == CurrentNumber(row, store)) {
        c.text = std::to_string(value);
        return c;
    }
    const std::string v = FormatIni(value * static_cast<double>(row.ini_scale));
    store.Set(row.keys[0], v);
    c.set.emplace_back(row.keys[0], v);
    c.text = std::to_string(value);
    return c;
}

// Defaults: every key of the tab's enabled rows goes back to its code default (removed from vr.ini;
// General: As the hunter, 0 cm).
inline Change Defaults(const Tab& tab, Store& store) {
    Change c{std::string{"defaults:"} + tab.name, "Defaults", {}, {}, false, false};
    // Snapshot enabled rows first: resetting a provider changes the enabled state
    // of dependent profile rows. Defaults must not depend on row order.
    std::vector<int> enabled;
    for (const int r : tab.rows) {
        if (r >= 0 && Enabled(Rows()[static_cast<size_t>(r)], store)) enabled.push_back(r);
    }
    for (const int r : enabled) {
        const Row& row = Rows()[static_cast<size_t>(r)];
        for (const char* key : row.keys) {
            if (key != nullptr) {
                store.Remove(key);
                c.removed.emplace_back(key);
                c.restart = c.restart || row.restart;
            }
        }
    }
    return c;
}

// ---- Navigation ---------------------------------------------------------------------------------

enum class Dir : unsigned char { Up, Down, Left, Right };

// ---- The pace of a held button (the presenter, every input frame) ----------------------------
//
// The stick's: a step at the push, then, held, a repeat after 0.4 s about 8 a second. The
// triggers' value steps use it as it is; the grips' tab steps only take the press (`repeat`
// false). The latches before it (the presenter's press / release thresholds) debounce the analog
// value as the stick's arm / fire thresholds do.
inline constexpr float RepeatDelayS = 0.4f;
inline constexpr float RepeatIntervalS = 0.12f;

struct PressRepeat {
    bool down{false};
    bool blocked{false}; // held through something else (the pointer's click): waits for a release
    float hold{0.0f};
    float next{RepeatDelayS};
    // One input frame: true at the press and, held (`repeat`), at each repeat.
    bool Step(bool pressed, float dt, bool repeat = true) {
        if (!pressed) {
            down = blocked = false;
            return false;
        }
        if (blocked) {
            return false;
        }
        if (!down) {
            down = true;
            hold = 0.0f;
            next = RepeatDelayS;
            return true;
        }
        if (!repeat) {
            return false;
        }
        hold += std::max(dt, 0.0f);
        if (hold >= next) {
            next = hold + RepeatIntervalS;
            return true;
        }
        return false;
    }
    // The button acts elsewhere this frame (the pointing trigger clicks): no step from this press.
    void Block(bool pressed) {
        down = pressed;
        blocked = pressed;
    }
};

// What the pointer is on (the right controller's ray on the page).
enum class Target : unsigned char {
    None,
    Page,       // on the page, not on anything that reacts
    Tab,        // index = tab
    TabPrev,    // the left grip's badge (LG)
    TabNext,    // the right grip's badge (RG)
    Row,        // index = slot: the row's label or value
    RowLeft,    // index = slot: its ◄
    RowRight,   // index = slot: its ►
    SliderBar,  // index = slot
    ButtonLeft,
    ButtonRight,
};

struct Hover {
    Target target{Target::None};
    int index{-1};
    bool operator==(const Hover&) const = default;
};

// What the page asks of its owner.
enum class Action : unsigned char { None, Close, ShowControls, RestartGraphics };

struct State {
    int tab{0};
    int focus{0};            // 0..4 a row slot, FocusLeftButton / FocusRightButton
    bool confirm{false};     // the Defaults question is up (the buttons read YES / NO)
    bool confirm_restart{false};
    int confirm_yes{0};      // 1 = YES selected (NO is preselected, as in the game)
    Hover hover{};
    int dragging{-1};        // the slot whose slider the trigger drags
    // A one-off help line from the owner, until the next move or change.
    std::string notice;
};

class Model {
public:
    using Log = std::function<void(const std::string&)>;

    explicit Model(Store& store_, Log log_ = {}) : store{store_}, log{std::move(log_)} {}

    const State& GetState() const {
        return state;
    }
    const Tab& CurrentTab() const {
        return Tabs()[static_cast<size_t>(state.tab)];
    }
    // The row in a slot of the current tab (nullptr when empty).
    const Row* RowAt(int slot) const {
        if (slot < 0 || slot >= Slots) {
            return nullptr;
        }
        const int r = CurrentTab().rows[static_cast<size_t>(slot)];
        return r >= 0 ? &Rows()[static_cast<size_t>(r)] : nullptr;
    }
    bool HasButton(int focus) const {
        if (state.confirm) {
            return focus == FocusLeftButton || focus == FocusRightButton;
        }
        const Tab& t = CurrentTab();
        return focus == FocusLeftButton    ? t.left.label != nullptr
               : focus == FocusRightButton ? t.right.label != nullptr
                                           : false;
    }
    bool Focusable(int focus) const {
        return focus < Slots ? RowAt(focus) != nullptr && Enabled(*RowAt(focus), store)
                             : HasButton(focus);
    }

    // Opening: the General tab's first row (as the game's pages open on their first row).
    void Reset() {
        store.Discard();
        state = State{};
        state.focus = FirstFocus();
    }

    // Changes made since the last call (the owner logs them, the tests check them).
    std::vector<Change> TakeChanges() {
        std::vector<Change> out;
        out.swap(changes);
        return out;
    }
    // Something on the page changed (the owner redraws).
    bool TakeDirty() {
        const bool d = dirty;
        dirty = false;
        return d;
    }
    void MarkDirty() {
        dirty = true;
    }

    // A help line shown until the next move or change.
    void SetNotice(const std::string& text) {
        state.notice = text;
        dirty = true;
    }

    Action Move(Dir dir) {
        if (!state.notice.empty()) {
            state.notice.clear();
            dirty = true;
        }
        if (state.confirm) {
            if (dir == Dir::Left || dir == Dir::Right) {
                state.confirm_yes = 1 - state.confirm_yes;
                dirty = true;
            }
            return Action::None;
        }
        if (dir == Dir::Up || dir == Dir::Down) {
            const int step = dir == Dir::Down ? 1 : -1;
            // The order: slots 0..4, then the button row (left, else right).
            std::vector<int> order;
            for (int s = 0; s < Slots; ++s) {
                if (Focusable(s)) {
                    order.push_back(s);
                }
            }
            const bool on_buttons = state.focus >= FocusLeftButton;
            const int button = Focusable(FocusLeftButton) ? FocusLeftButton : FocusRightButton;
            if (Focusable(button)) {
                order.push_back(on_buttons ? state.focus : button);
            }
            if (order.empty()) {
                return Action::None;
            }
            auto it = std::find(order.begin(), order.end(), state.focus);
            int i = it == order.end() ? 0 : static_cast<int>(it - order.begin());
            i = ((i + step) % static_cast<int>(order.size()) + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
            if (order[static_cast<size_t>(i)] != state.focus) {
                state.focus = order[static_cast<size_t>(i)];
                dirty = true;
            }
            return Action::None;
        }
        const int delta = dir == Dir::Right ? 1 : -1;
        if (state.focus >= FocusLeftButton) {
            const int other = state.focus == FocusLeftButton ? FocusRightButton : FocusLeftButton;
            if (Focusable(other) && ((delta > 0) == (other == FocusRightButton))) {
                state.focus = other;
                dirty = true;
            }
            return Action::None;
        }
        if (const Row* row = RowAt(state.focus)) {
            Apply(Step(*row, store, delta));
        }
        return Action::None;
    }

    // A trigger: the highlighted row's next (+1, the right trigger) or previous value, exactly as
    // the stick pushed right / left (on the button row: the other button; in the Defaults
    // question: YES / NO).
    Action Value(int delta) {
        return delta == 0 ? Action::None : Move(delta > 0 ? Dir::Right : Dir::Left);
    }

    Action Confirm() {
        if (!state.notice.empty()) {
            state.notice.clear();
            dirty = true;
        }
        if (state.confirm) {
            const bool yes = state.confirm_yes == 1;
            state.confirm = false;
            state.focus = FocusRightButton;
            dirty = true;
            if (yes && state.confirm_restart) {
                state.confirm_restart = false;
                store.Accept();
                return Action::RestartGraphics;
            }
            if (yes) {
                Apply(Defaults(CurrentTab(), store));
            } else if (log) {
                log("VR page: Defaults: NO (nothing changed)");
            }
            return Action::None;
        }
        if (state.focus >= FocusLeftButton) {
            const Button& b = state.focus == FocusLeftButton ? CurrentTab().left : CurrentTab().right;
            switch (b.action) {
            case ButtonAction::ShowControls:
                return Action::ShowControls;
            case ButtonAction::RestartGraphics:
                state.confirm = true;
                state.confirm_restart = true;
                state.confirm_yes = 0;
                dirty = true;
                return Action::None;
            case ButtonAction::Defaults:
                state.confirm_restart = false;
                state.confirm = true;
                state.confirm_yes = 0;
                dirty = true;
                return Action::None;
            case ButtonAction::AcceptGraphics:
                if (store.Accept()) {
                    if (log)
                        log("VR page: graphics preferences accepted.");
                    SetNotice("Graphics saved. Resolution changes take effect after SAVE AND RESTART.");
                } else
                    SetNotice("No pending graphics changes.");
                return Action::None;
            case ButtonAction::None:
                return Action::None;
            }
        }
        return Action::None; // A on a row: nothing (its value changes with left / right)
    }

    // B: closes the Defaults question, else the page.
    Action Back() {
        if (state.confirm) {
            state.confirm = false;
            state.focus = FocusRightButton;
            dirty = true;
            return Action::None;
        }
        store.Discard();
        return Action::Close;
    }

    void TabStep(int delta) {
        const int count = static_cast<int>(Tabs().size());
        SetTab(((state.tab + delta) % count + count) % count);
    }
    void SetTab(int tab) {
        if (tab == state.tab || tab < 0 ||
            tab >= static_cast<int>(Tabs().size())) {
            return;
        }
        state.tab = tab;
        state.notice.clear();
        state.confirm = false;
        state.focus = FirstFocus();
        state.dragging = -1;
        dirty = true;
    }

    // ---- The pointer ----
    void SetHover(Hover h) {
        if (h != state.hover) {
            state.hover = h;
            // Hovering moves the selection glow (rows and buttons).
            if (h.target == Target::Row || h.target == Target::RowLeft ||
                h.target == Target::RowRight || h.target == Target::SliderBar) {
                if (Focusable(h.index) && !state.confirm) {
                    state.focus = h.index;
                }
            } else if (h.target == Target::ButtonLeft && Focusable(FocusLeftButton)) {
                state.focus = FocusLeftButton;
                state.confirm_yes = 1;
            } else if (h.target == Target::ButtonRight && Focusable(FocusRightButton)) {
                state.focus = FocusRightButton;
                state.confirm_yes = 0;
            }
            dirty = true;
        }
    }
    // The trigger pressed with the ray on the page (`fraction`: along a slider's bar).
    Action Click(float fraction = 0.0f) {
        const Hover h = state.hover;
        if (!state.notice.empty()) {
            state.notice.clear();
            dirty = true;
        }
        switch (h.target) {
        case Target::Tab:
            SetTab(h.index);
            return Action::None;
        case Target::TabPrev:
            TabStep(-1);
            return Action::None;
        case Target::TabNext:
            TabStep(1);
            return Action::None;
        case Target::RowLeft:
        case Target::RowRight:
            if (const Row* row = RowAt(h.index);
                row != nullptr && !state.confirm && Enabled(*row, store)) {
                state.focus = h.index;
                Apply(Step(*row, store, h.target == Target::RowRight ? 1 : -1));
            }
            return Action::None;
        case Target::Row:
            if (const Row* row = RowAt(h.index);
                row != nullptr && !state.confirm && Enabled(*row, store)) {
                state.focus = h.index;
                if (row->widget == Widget::Choice) {
                    Apply(Step(*row, store, 1));
                }
            }
            return Action::None;
        case Target::SliderBar:
            if (const Row* row = RowAt(h.index);
                row != nullptr && !state.confirm && Enabled(*row, store)) {
                state.focus = h.index;
                state.dragging = h.index;
                Apply(SetFraction(*row, store, fraction));
            }
            return Action::None;
        case Target::ButtonLeft:
        case Target::ButtonRight:
            if (state.confirm) {
                state.confirm_yes = h.target == Target::ButtonLeft ? 1 : 0;
            } else {
                state.focus = h.target == Target::ButtonLeft ? FocusLeftButton : FocusRightButton;
            }
            return Confirm();
        default:
            return Action::None;
        }
    }
    // The trigger held after a click on a slider's bar.
    void Drag(float fraction) {
        if (state.dragging < 0) {
            return;
        }
        if (const Row* row = RowAt(state.dragging)) {
            Apply(SetFraction(*row, store, fraction));
        }
    }
    void Release() {
        state.dragging = -1;
    }

    bool GraphicsPending() const {
        return store.Pending();
    }
    void DiscardGraphics() {
        store.Discard();
        dirty = true;
    }

    Store& GetStore() {
        return store;
    }

private:
    int FirstFocus() const {
        for (int s = 0; s < Slots; ++s) {
            if (Focusable(s)) {
                return s;
            }
        }
        return Focusable(FocusLeftButton) ? FocusLeftButton : FocusRightButton;
    }

    void Apply(Change c) {
        if (c.set.empty() && c.removed.empty()) {
            return;
        }
        dirty = true;
        if (log) {
            std::string what;
            for (const auto& [k, v] : c.set) {
                what += (what.empty() ? "" : ", ") + k + "=" + v;
            }
            for (const auto& k : c.removed) {
                what += (what.empty() ? "" : ", ") + k + " (default)";
            }
            log("VR page: " + c.row + " = " + c.text + " (" + what + ")" +
                ((std::any_of(c.set.begin(), c.set.end(),
                              [](const auto& kv) { return kv.first.starts_with("graphics_"); }) ||
                  std::any_of(c.removed.begin(), c.removed.end(),
                              [](const auto& k) { return k.starts_with("graphics_"); }))
                     ? " [pending ACCEPT]"
                 : c.placeholder ? " [stored; applied by a later build]"
                 : c.restart     ? " [after a restart]"
                                 : " [live]"));
        }
        changes.push_back(std::move(c));
    }

    mutable GraphicsDraftStore store;
    Log log;
    State state{};
    std::vector<Change> changes;
    bool dirty{true};
};

} // namespace VR::Page
