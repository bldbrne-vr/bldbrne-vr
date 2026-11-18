// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

// Play style (vr.ini play_style, the launcher's Play page; the tester, 2026-11-08: "people who want
// to play the game traditionally in VR"): motion controls (the default), or a gamepad in first or
// third person. A gamepad style sets the settings below while it runs, on top of the player's own,
// and never writes them: vr.ini keeps the motion settings, so going back to Motion brings them back
// as they were. The VR page and the launcher show those rows locked.
//
// What needs tracked hands, and what stands in for it (the audit of 2026-11-08): viscerals waiting
// for a pull, the summon bell's ring, the levers' hidden prompt, gun and tool
// aim from the hands, the arms following the controllers, the wrist gauge, room-scale steps writing
// the stick, and the head-only combat (lean and duck i-frames, lean dodge, physical evasion).
namespace VR::PlayStyle {

enum class Style : int { Motion = 0, PadFirst = 1, PadThird = 2 };

// vr.ini words: motion, gamepad_first, gamepad_third (0, 1, 2 too).
[[nodiscard]] inline std::optional<Style> Parse(std::string_view value) {
    if (value == "motion" || value == "0") return Style::Motion;
    if (value == "gamepad_first" || value == "1") return Style::PadFirst;
    if (value == "gamepad_third" || value == "2") return Style::PadThird;
    return std::nullopt;
}

[[nodiscard]] constexpr const char* Name(Style style) {
    return style == Style::PadFirst   ? "gamepad_first"
           : style == Style::PadThird ? "gamepad_third"
                                      : "motion";
}

struct Forced {
    std::string_view key;
    std::string_view value; // in vr.ini's words (ParseSettingsLine reads it)
};

// Both gamepad styles. The game's own buttons do everything; nothing waits for a hand.
// (The lamp rest & travel page, lamp_travel, is the player's own in first person: the gamepad
// drives VR pages, D-pad / left stick, Cross, Circle, L1 / R1; the tester, 2026-11-09. Third
// person sets it off below.)
inline constexpr std::array<Forced, 25> GamepadForced{{
    {"controls", "classic"},           // buttons only: no swings, lean dodge, lean / duck i-frames
    {"visceral_pull", "0"},            // a visceral never waits ~20 s for a pull
    {"physical_summon_bell", "0"},     // the sign's press summons, no ring needed
    {"physical_levers", "0"},          // the game's own lever prompt
    {"roomscale_walk", "0"},           // head steps never write the gamepad's stick
    {"physical_evasion", "0"},         // the game's own dodges only
    {"lean_iframes_ms", "0"},
    {"duck_iframes_ms", "0"},
    {"lean_dodge", "0"},
    {"shoulder_switch", "0"},
    {"arm_ik", "0"},                   // the arms are the game's animation
    {"gun_aim", "0"},                  // the game's own aim
    {"tool_aim", "0"},
    {"hand_rig", "0"},
    {"corpse_hands", "0"},
    {"laser", "0"},
    {"controller_models", "0"},
    {"hud_vitals", "classic"},         // the game's HUD, not the wrist gauge
    {"ui_vr_buttons", "0"},            // the game's prompts with PlayStation buttons
    {"ui_counters", "always"},         // Echoes and Insight without looking down
    {"ui_counters_pos", "under_status"}, // ... under the health bar
    // The review of 2026-11-08: still hand-driven after the above.
    {"whip", "canned"},          // the game's own whip clips: a physical Amygdalan Arm in form B
                                 // follows the controller's (zero) speed through the game's attacks
    {"gun_wrist_reload", "0"},   // shots fired from the pad never wait for a wrist flick
    {"pose_gestures", "0"},      // the Doll's bow from resting controllers and a look down
    {"ghost_hands", "0"},        // no see-through hands where the controllers lie
}};
inline constexpr std::array<Forced, 1> FirstPersonForced{{{"first_person", "1"}}};
// Third person: the game's camera already shows the hunter, so its interactions play in VR as the
// game animates them (2026-11-08: "it doesn't make sense to switch to flat"). No scene on
// the flat screen (doors, levers, lamps and arrivals, ladders, NPCs, the Doll, fog gates, throws)
// and no ladder skip. Chests, doors, lamp lighting and pickups already play the hunter's animation
// in third person (vr_interactions and pickup_instant are first person only). The game's own lamp
// menu too (2026-11-09: our lamp page in gamepad first person only).
inline constexpr std::array<Forced, 5> ThirdPersonForced{{
    {"first_person", "0"},
    {"cinematic", "0"},
    {"cinematic_lamps", "0"},
    {"cinematic_ladder_skip", "0"},
    {"lamp_travel", "0"},
}};

// Calls `f(key, value)` for each setting the style sets (none for Motion).
template <class F> void ForEach(Style style, F&& f) {
    if (style == Style::Motion) return;
    for (const Forced& forced : GamepadForced) f(forced.key, forced.value);
    const std::span<const Forced> view = style == Style::PadThird
                                             ? std::span<const Forced>(ThirdPersonForced)
                                             : std::span<const Forced>(FirstPersonForced);
    for (const Forced& forced : view) f(forced.key, forced.value);
}

// The value `style` sets for `key`, if it sets one.
[[nodiscard]] inline std::optional<std::string_view> ForcedValue(Style style,
                                                                 std::string_view key) {
    std::optional<std::string_view> out;
    ForEach(style, [&](std::string_view k, std::string_view v) {
        if (k == key) out = v;
    });
    return out;
}

[[nodiscard]] inline bool IsForced(Style style, std::string_view key) {
    return ForcedValue(style, key).has_value();
}

} // namespace VR::PlayStyle
