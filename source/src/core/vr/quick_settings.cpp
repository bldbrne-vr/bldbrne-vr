// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <ostream>

#include "core/vr/quick_settings.h"

namespace VR::Quick {

namespace {

bool Flag(const std::string& value) {
    return value == "1" || value == "true";
}

} // Anonymous namespace

QuickSettings& GetQuickSettings() {
    static QuickSettings settings;
    return settings;
}

bool ParseQuickSetting(const std::string& key, const std::string& value) {
    auto& s = GetQuickSettings();
    const auto f = [&value] { return std::stof(value); };
    if (key == "quick_items") {
        s.quick_items = Flag(value);
    } else if (key == "quick_arc") {
        s.quick_arc = Flag(value);
    } else if (key == "quick_arc_l3") {
        s.arc_l3 = Flag(value);
    } else if (key == "quick_arc_l3_tap_ms") {
        s.l3_tap_ms = std::clamp(f(), 100.0f, 2000.0f);
    } else if (key == "quick_arc_dwell_ms") {
        s.dwell_ms = std::clamp(f(), 0.0f, 500.0f);
    } else if (key == "quick_trigger_release") {
        s.trigger_release = std::clamp(f(), 0.05f, 0.44f);
    } else if (key == "quick_arc_reopen_s") {
        s.reopen_s = std::clamp(f(), 0.0f, 10.0f);
    } else if (key == "holster_mode") {
        s.holster_mode = std::clamp(static_cast<int>(std::lround(f())), 0, 2);
    } else if (key == "holster_right" || key == "holster_left" || key == "holster_chest") {
        // Accept legacy keys without restoring remappable vials or overriding inventory order.
        return true;
    } else if (key == "holster_haptics") {
        s.holster_haptics = Flag(value);
    } else if (key == "holster_height_cm") {
        s.holster_height_cm = std::clamp(f(), -20.0f, 20.0f);
    } else if (key == "holster_width_cm") {
        s.holster_width_cm = std::clamp(f(), -10.0f, 15.0f);
    } else if (key == "holster_forward_cm") {
        s.holster_forward_cm = std::clamp(f(), -10.0f, 20.0f);
    } else if (key == "holster_reach_cm") {
        s.holster_reach_cm = std::clamp(f(), 6.0f, 18.0f);
    } else if (key == "belt_vial") {
        s.belt_vial = Flag(value);
    } else if (key == "belt_vial_zone") {
        s.belt_vial_zone = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "quick_arc_radius_m") {
        s.radius_m = std::clamp(f(), 0.08f, 0.30f);
    } else if (key == "quick_arc_card_m") {
        s.card_m = std::clamp(f(), 0.03f, 0.10f);
    } else if (key == "quick_arc_span_deg") {
        s.span_deg = std::clamp(f(), 60.0f, 200.0f);
    } else if (key == "quick_arc_hover_scale") {
        s.hover_scale = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "quick_arc_pick") {
        s.pick_sector = value == "sector" || value == "1";
    } else if (key == "quick_arc_grace_ms") {
        s.grace_ms = std::clamp(f(), 0.0f, 500.0f);
    } else if (key == "quick_arc_nearest") {
        s.nearest = std::clamp(f(), 0.0f, 2.5f);
    } else if (key == "quick_arc_hide_gun_open") {
        s.hide_gun_open = Flag(value);
    } else if (key == "quick_arc_hover_xray") {
        s.hover_xray = Flag(value);
    } else if (key == "quick_arc_hidden_xray") {
        s.hidden_xray = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "quick_arc_quick_draw") {
        s.quick_draw = Flag(value);
    } else if (key == "quick_arc_select") {
        s.select = Flag(value);
    } else if (key == "quick_arc_use_commit_ms") {
        s.use_commit_ms = std::clamp(f(), 0.0f, 500.0f);
    } else if (key == "quick_arc_use_cooldown_ms") {
        s.use_cooldown_ms = std::clamp(f(), 0.0f, 5000.0f);
    } else if (key == "quick_arc_trigger_parry") {
        s.trigger_parry = Flag(value);
    } else if (key == "quick_arc_hide_gun") {
        s.hide_gun = Flag(value);
    } else if (key == "quick_arc_labels") {
        s.labels = Flag(value);
    } else if (key == "quick_arc_icons") {
        s.icons = Flag(value);
    } else if (key == "quick_arc_idle_close_s") {
        s.idle_close_s = std::clamp(f(), 0.0f, 120.0f);
    } else if (key == "quick_arc_stack") {
        s.stack = Flag(value);
    } else if (key == "quick_arc_regrab_s") {
        s.regrab_s = std::clamp(f(), 0.2f, 5.0f);
    } else if (key == "quick_held_scale") {
        s.held_scale = std::clamp(f(), 0.5f, 3.0f);
    } else if (key == "held_grip") {
        s.held_grip = Flag(value);
    } else if (key == "quick_arc_haptics") {
        s.haptics = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "quick_instant") {
        s.instant = Flag(value);
    } else if (key == "quick_use_mouth") {
        s.use_mouth = Flag(value);
    } else if (key == "quick_mouth_radius_m") {
        s.mouth_radius_m = std::clamp(f(), 0.05f, 0.20f);
    } else if (key == "quick_mouth_down_m") {
        s.mouth_down_m = std::clamp(f(), 0.0f, 0.20f);
    } else if (key == "quick_mouth_ahead_m") {
        s.mouth_ahead_m = std::clamp(f(), -0.05f, 0.20f);
    } else if (key == "quick_mouth_dwell_ms") {
        s.mouth_dwell_ms = std::clamp(f(), 0.0f, 1000.0f);
    } else if (key == "quick_mouth_sound") {
        s.mouth_sound = Flag(value);
    } else if (key == "quick_use_sounds") {
        s.use_sounds = Flag(value);
    } else if (key == "quick_use_whoosh") {
        const float volume = f();
        s.use_whoosh = std::isfinite(volume) ? std::clamp(volume, 0.0f, 2.0f) : 0.0f;
    } else if (key == "quick_use_wipe") {
        s.use_wipe = Flag(value);
    } else if (key == "quick_wipe_reach_cm") {
        s.wipe_reach_cm = std::clamp(f(), 2.0f, 20.0f);
    } else if (key == "quick_wipe_share") {
        s.wipe_share = std::clamp(f(), 0.1f, 1.0f);
    } else if (key == "quick_wipe_window_ms") {
        s.wipe_window_ms = std::clamp(f(), 200.0f, 2000.0f);
    } else if (key == "quick_wipe_min_cm") {
        s.wipe_min_cm = std::clamp(f(), 3.0f, 25.0f);
    } else if (key == "quick_wipe_own") {
        s.wipe_own = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "quick_wipe_max_cm") {
        s.wipe_max_cm = std::clamp(f(), 5.0f, 40.0f);
    } else if (key == "quick_wipe_turn_deg") {
        s.wipe_turn_deg = std::clamp(f(), 0.0f, 180.0f);
    } else if (key == "quick_wipe_cue") {
        // On / off: the ticks (an older vr.ini's 2 or 3 also reads as on).
        s.wipe_cue = value == "true" || (value != "false" && std::lround(f()) != 0);
    } else if (key == "quick_wipe_cue_cm") {
        s.wipe_cue_cm = std::clamp(f(), 5.0f, 60.0f);
    } else if (key == "quick_wipe_rub") {
        s.wipe_rub = Flag(value);
    } else if (key == "quick_wipe_blade_cm") {
        s.wipe_blade_cm = std::clamp(f(), 0.0f, 15.0f);
    } else if (key == "quick_wipe_touch_cm") {
        s.wipe_touch_cm = std::clamp(f(), 0.5f, 15.0f);
    } else if (key == "quick_wipe_rub_cm") {
        s.wipe_rub_cm = std::clamp(f(), 5.0f, 60.0f);
    } else if (key == "quick_wipe_rub_ms") {
        s.wipe_rub_ms = std::clamp(f(), 300.0f, 4000.0f);
    } else if (key == "quick_wipe_stroke_cm") {
        s.wipe_stroke_cm = std::clamp(f(), 1.0f, 20.0f);
    } else if (key == "quick_wipe_blade_mps") {
        s.wipe_blade_mps = std::clamp(f(), 0.2f, 10.0f);
    } else if (key == "quick_hold_use") {
        s.hold_use = Flag(value);
    } else if (key == "quick_hold_use_s") {
        s.hold_use_s = std::clamp(f(), 0.3f, 5.0f);
    } else if (key == "quick_use_crush") {
        s.use_crush = Flag(value);
    } else if (key == "quick_crush_press") {
        s.crush_press = std::clamp(f(), 0.6f, 1.0f);
    } else if (key == "quick_crush_hold_ms") {
        s.crush_hold_ms = std::clamp(f(), 30.0f, 600.0f);
    } else if (key == "quick_use_shake") {
        s.use_shake = Flag(value);
    } else if (key == "quick_shake_choir_bell") {
        s.shake_choir_bell = Flag(value);
    } else if (key == "quick_shake_strokes") {
        s.shake_strokes = static_cast<int>(std::lround(std::clamp(f(), 2.0f, 6.0f)));
    } else if (key == "quick_shake_window_ms") {
        s.shake_window_ms = std::clamp(f(), 400.0f, 2000.0f);
    } else if (key == "quick_shake_wrist") {
        s.shake_wrist = std::clamp(f(), 2.0f, 15.0f);
    } else if (key == "quick_shake_arm") {
        s.shake_arm = std::clamp(f(), 0.4f, 3.0f);
    } else if (key == "quick_use_stab") {
        s.use_stab = Flag(value);
    } else if (key == "quick_music_box_crank") {
        s.music_box_crank = Flag(value);
    } else if (key == "music_box_grab_cm") {
        s.music_box_grab_cm = std::clamp(f(), 2.0f, 15.0f);
    } else if (key == "music_box_turns") {
        s.music_box_turns = std::clamp(f(), 0.25f, 4.0f);
    } else if (key == "music_box_replay_s") {
        s.music_box_replay_s = std::clamp(f(), 0.5f, 30.0f);
    } else if (key == "quick_throw") {
        s.throws = Flag(value);
    } else if (key == "throw_min_speed") {
        s.throw_min_speed = std::clamp(f(), 0.3f, 6.0f);
    } else if (key == "throw_gain") {
        s.throw_gain = std::clamp(f(), 0.5f, 8.0f);
    } else if (key == "throw_gain_curve") {
        s.throw_gain_curve = std::clamp(f(), 0.5f, 1.2f);
    } else if (key == "throw_fresh_pose") {
        s.throw_fresh_pose = Flag(value);
    } else if (key == "throw_rise_wait_ms") {
        s.throw_rise_wait_ms = std::clamp(f(), 0.0f, 60.0f);
    } else if (key == "throw_peak_turn_deg") {
        s.throw_peak_turn_deg = std::clamp(f(), 0.0f, 75.0f);
    } else if (key == "throw_lift_deg") {
        s.throw_lift_deg = std::clamp(f(), 0.0f, 15.0f);
    } else if (key == "throw_cap_flat_deg") {
        s.throw_cap_flat_deg = std::clamp(f(), 0.0f, 45.0f);
    } else if (key == "throw_sound") {
        s.throw_sound = Flag(value);
    } else if (key == "molotov_burst") {
        s.molotov_burst = Flag(value);
    } else if (key == "molotov_burst_deg") {
        s.molotov_burst_deg = std::clamp(f(), 1.0f, 45.0f);
    } else if (key == "molotov_burst_bullet") {
        s.molotov_burst_bullet = static_cast<int>(std::lround(f()));
    } else if (key == "molotov_burst_radius_m") {
        s.molotov_burst_radius_m = std::clamp(f(), 0.0f, 6.0f);
    } else if (key == "molotov_burst_ground") {
        s.molotov_burst_ground = Flag(value);
    } else if (key == "throw_haptic_scale") {
        s.throw_haptic_scale = Flag(value);
    } else if (key == "throw_peak_window_s") {
        s.throw_peak_window_s = std::clamp(f(), 0.0f, 0.6f);
    } else if (key == "throw_push_factor") {
        s.throw_push_factor = std::clamp(f(), 0.3f, 1.0f);
    } else if (key == "throw_inherit_motion") {
        s.throw_inherit_motion = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "throw_physics") {
        s.throw_physics = Flag(value);
    } else if (key == "throw_from_hand") {
        s.throw_from_hand = Flag(value);
    } else if (key == "throw_gravity") {
        s.throw_gravity = std::clamp(f(), 0.0f, 30.0f);
    } else if (key == "throw_spin") {
        s.throw_spin = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "throw_range_factor") {
        s.throw_range_factor = std::clamp(f(), 0.5f, 3.0f);
    } else if (key == "throw_cap_knee") {
        s.throw_cap_knee = std::clamp(f(), 0.0f, 0.95f);
    } else if (key == "throw_assist") {
        s.throw_assist = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "throw_assist_deg") {
        s.throw_assist_deg = std::clamp(f(), 0.0f, 45.0f);
    } else if (key == "throw_assist_knife_deg") {
        s.throw_assist_knife_deg = std::clamp(f(), 0.0f, 45.0f);
    } else if (key == "throw_assist_unlocked") {
        s.throw_assist_unlocked = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "knife_assist") {
        s.knife_assist = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "knife_assist_deg") {
        s.knife_assist_deg = std::clamp(f(), 0.0f, 30.0f);
    } else if (key == "inframe") {
        s.inframe = Flag(value);
    } else if (key == "inframe_exposure") {
        s.inframe_exposure = std::clamp(f(), 0.2f, 4.0f);
    } else if (key == "inframe_hand_sync") {
        s.inframe_hand_sync = Flag(value);
    } else {
        return false;
    }
    return true;
}

void WriteQuickDefaults(std::ostream& out) {
    const QuickSettings d{};
    out << "# ---- Quick items: Y opens your quick items above the hunter's left hand ----\n"
        << "# game side (read, select, use and throw quick items); 0 = Y is Square\n"
        << "quick_items=" << (d.quick_items ? 1 : 0) << "\n"
        << "# Y opens the arc; 0 = Y is the game's Square exactly as before\n"
        << "quick_arc=" << (d.quick_arc ? 1 : 0) << "\n"
        << "# a tap of the left stick click (not sprinting: that jumps) opens / closes the arc "
           "like "
           "Y, the thumb still on the stick; 0 = the tap is L3 to the game. A release after this "
           "long (ms) is no tap\n"
        << "quick_arc_l3=" << (d.arc_l3 ? 1 : 0) << "\n"
        << "quick_arc_l3_tap_ms=" << d.l3_tap_ms << "\n"
        << "# left_scheme=2 (the left grip opens the arc, the left trigger takes the card the "
           "hand is on): the trigger\n# takes only a card lit this long (ms); a throwable flies "
           "when the trigger is let go below\n# quick_trigger_release (0..1); after a throw the "
           "arc is back this long for the next one (s, 0 = off: it closes)\n"
        << "quick_arc_dwell_ms=" << d.dwell_ms << "\n"
        << "quick_trigger_release=" << d.trigger_release << "\n"
        << "quick_arc_reopen_s=" << d.reopen_s << "\n"
        << "# left_scheme=2: the left grip at the left hip (beside the lap seated) draws a Blood "
           "Vial, as X;\n# belt_vial_zone scales the hip's box\n"
        << "holster_mode=" << d.holster_mode << "\n"
        << "holster_haptics=" << d.holster_haptics << "\n"
        << "holster_height_cm=" << d.holster_height_cm << "\n"
        << "holster_width_cm=" << d.holster_width_cm << "\n"
        << "holster_forward_cm=" << d.holster_forward_cm << "\n"
        << "holster_reach_cm=" << d.holster_reach_cm << "\n"
        << "belt_vial=" << (d.belt_vial ? 1 : 0) << "\n"
        << "belt_vial_zone=" << d.belt_vial_zone << "\n"
        << "# arc radius around the hand, card size (m) and the arc's span (degrees)\n"
        << "quick_arc_radius_m=" << d.radius_m << "\n"
        << "quick_arc_card_m=" << d.card_m << "\n"
        << "quick_arc_span_deg=" << d.span_deg << "\n"
        << "# hover volume around each card (x)\n"
        << "quick_arc_hover_scale=" << d.hover_scale << "\n"
        << "# hover = the hunter's fist on a card picks it; sector = point the hand at it from "
           "where the arc opened\n"
        << "quick_arc_pick=" << (d.pick_sector ? "sector" : "hover") << "\n"
        << "# a squeeze just after the hand left a card still takes it (ms) / one with no card lit "
           "takes the clearly nearest card up to this x the hover volume (0 = off)\n"
        << "quick_arc_grace_ms=" << d.grace_ms << "\n"
        << "quick_arc_nearest=" << d.nearest << "\n"
        << "# the left weapon is hidden while the arc is open (0: the gun stays in the hand and "
           "the trigger fires it at the pull)\n"
        << "quick_arc_hide_gun_open=" << (d.hide_gun_open ? 1 : 0) << "\n"
        << "# the hovered card is shown through the hunter's fist where the hand covers it\n"
        << "quick_arc_hover_xray=" << (d.hover_xray ? 1 : 0) << "\n"
        << "# every card a wall, a pillar or the hunter's arm hides is shown through it at this "
           "opacity (0 = hidden)\n"
        << "quick_arc_hidden_xray=" << d.hidden_xray << "\n"
        << "# Y then the left grip at once (no reach) takes the game's selected item\n"
        << "quick_arc_quick_draw=" << (d.quick_draw ? 1 : 0) << "\n"
        << "# a grab also selects the item in the game (its HUD slot)\n"
        << "quick_arc_select=" << (d.select ? 1 : 0) << "\n"
        << "# a use is a pull of the left trigger held this long (ms; a shorter tap does "
           "nothing) / between two uses (ms)\n"
        << "quick_arc_use_commit_ms=" << d.use_commit_ms << "\n"
        << "quick_arc_use_cooldown_ms=" << d.use_cooldown_ms << "\n"
        << "# left trigger with a throwable in hand, or a tap with a usable: put it away and fire "
           "the gun (L2); 0 = the gun never fires while an item is in the hand\n"
        << "quick_arc_trigger_parry=" << (d.trigger_parry ? 1 : 0) << "\n"
        << "# the left weapon is hidden while an item is in the hand\n"
        << "quick_arc_hide_gun=" << (d.hide_gun ? 1 : 0) << "\n"
        << "# the hovered item's name and the counts / the game's item icons on the cards\n"
        << "quick_arc_labels=" << (d.labels ? 1 : 0) << "\n"
        << "quick_arc_icons=" << (d.icons ? 1 : 0) << "\n"
        << "# an open arc closes after this long without a grab (s, 0 = never)\n"
        << "quick_arc_idle_close_s=" << d.idle_close_s << "\n"
        << "# after a throw or a use, squeeze the left grip again within this long (s) for the "
           "next one\n"
        << "quick_arc_stack=" << (d.stack ? 1 : 0) << "\n"
        << "quick_arc_regrab_s=" << d.regrab_s << "\n"
        << "# the held item's size (the game's own models, life size = 1)\n"
        << "quick_held_scale=" << d.held_scale << "\n"
        << "# the hunter's left fingers close round a held item or the vial (0 = the animation's "
           "hand)\n"
        << "held_grip=" << (d.held_grip ? 1 : 0) << "\n"
        << "# quick-item haptics strength (0 = none)\n"
        << "quick_arc_haptics=" << d.haptics << "\n"
        << "# usables whose effect is instant skip the animation; 0 = always the game's animation\n"
        << "quick_instant=" << (d.instant ? 1 : 0) << "\n"
        << "# pellets and antidotes (eaten), Blue / Lead Elixir and Sedative (drunk) stay in the "
           "hand when taken\n# and are used when you bring them to your mouth (let go away from "
           "it: back); 0 = used at the pull.\n# The mouth: a ball of this radius (m) round a "
           "point this far below and in front of your eyes (m),\n# kept there this long (ms); "
           "the game's own drinking sound on a drink\n"
        << "quick_use_mouth=" << (d.use_mouth ? 1 : 0) << "\n"
        << "quick_mouth_radius_m=" << d.mouth_radius_m << "\n"
        << "quick_mouth_down_m=" << d.mouth_down_m << "\n"
        << "quick_mouth_ahead_m=" << d.mouth_ahead_m << "\n"
        << "quick_mouth_dwell_ms=" << d.mouth_dwell_ms << "\n"
        << "quick_mouth_sound=" << (d.mouth_sound ? 1 : 0) << "\n"
        << "# An item used at once (eaten, drunk, wiped, crushed, held to use, stabbed, the Blood "
           "Vial) plays the game's\n# own sound of that use (the closest one where its animation "
           "has none); 0 = only the drink above\n"
        << "quick_use_sounds=" << (d.use_sounds ? 1 : 0) << "\n"
        << "# Quiet rising whoosh while holding/squeezing an item to use; volume 0..2, 0 = off\n"
        << "quick_use_whoosh=" << d.use_whoosh << "\n"
        << "# Fire / Bolt Paper and the Empty Phantasm Shell stay in the left hand when taken and "
           "are applied when you\n# wipe them along your weapon's blade (within this many cm of "
           "it, along this share of its length\n# but at least min_cm, within this long, ms; the "
           "paper's own move at least this share of the\n# stroke: a thrust or a swing of the "
           "blade past the paper is no wipe; at most max_cm; the blade turning at most\n# "
           "turn_deg over it: a swing is no wipe); let go away from the blade: back. 0 = used at "
           "the pull.\n# The first version: reach 7, share 0.4, window 800, min 8, max 25, own 0, "
           "turn 0, cue 0\n"
        << "quick_use_wipe=" << (d.use_wipe ? 1 : 0) << "\n"
        << "quick_wipe_reach_cm=" << d.wipe_reach_cm << "\n"
        << "quick_wipe_share=" << d.wipe_share << "\n"
        << "quick_wipe_window_ms=" << d.wipe_window_ms << "\n"
        << "quick_wipe_min_cm=" << d.wipe_min_cm << "\n"
        << "quick_wipe_own=" << d.wipe_own << "\n"
        << "quick_wipe_max_cm=" << d.wipe_max_cm << "\n"
        << "quick_wipe_turn_deg=" << d.wipe_turn_deg << "\n"
        << "# While a paper is in the hand, where to rub: 1 = soft ticks on the paper hand, "
           "quickening as it nears the\n# blade (from this many cm away), 0 = none\n"
        << "quick_wipe_cue=" << (d.wipe_cue ? 1 : 0) << "\n"
        << "quick_wipe_cue_cm=" << d.wipe_cue_cm << "\n"
        << "# The rub: the paper you see (its card) within touch_cm of the blade's surface (its "
           "axis with blade_cm round\n# it), rubbed along it back and forth: strokes of at least "
           "stroke_cm adding up to rub_cm within rub_ms,\n# the blade itself under the paper "
           "slower than blade_mps (a swing past a still paper is no rub); one long\n# stroke "
           "(share / min_cm / max_cm above) still applies at once. 0 = the one-way stroke from "
           "the fist (reach_cm,\n# window_ms above)\n"
        << "quick_wipe_rub=" << (d.wipe_rub ? 1 : 0) << "\n"
        << "quick_wipe_blade_cm=" << d.wipe_blade_cm << "\n"
        << "quick_wipe_touch_cm=" << d.wipe_touch_cm << "\n"
        << "quick_wipe_rub_cm=" << d.wipe_rub_cm << "\n"
        << "quick_wipe_rub_ms=" << d.wipe_rub_ms << "\n"
        << "quick_wipe_stroke_cm=" << d.wipe_stroke_cm << "\n"
        << "quick_wipe_blade_mps=" << d.wipe_blade_mps << "\n"
        << "# coldbloods, Madman's Knowledge, NPC bloods, marks, bells (the misc items whose use "
           "cannot be taken back)\n# stay in the hand while you keep the trigger held (scheme 1: "
           "the grip): a ring fills over this long\n# (s), then the item is used; let go before: "
           "back, nothing used. 0 = used at the pull\n"
        << "quick_hold_use=" << (d.hold_use ? 1 : 0) << "\n"
        << "quick_hold_use_s=" << d.hold_use_s << "\n"
        << "# natural gestures, the faster way (the hold stays): CRUSH coldbloods, Madman's "
           "Knowledge, the Blood Dreg and Vermin\n# in the fist (squeeze the other button hard: "
           "scheme 2 the grip, 1 the trigger; at least this, this long, ms),\n# SHAKE a bell "
           "(this many back-and-forth strokes within this long, ms; a wrist flick of this many "
           "rad/s\n# or a forearm shake of this many m/s; the Choir Bell too with "
           "quick_shake_choir_bell=1), STAB the NPC bloods\n# and Iosefka's vial into your wrist "
           "or body like the Blood Vial. 0 = the hold only\n"
        << "quick_use_crush=" << (d.use_crush ? 1 : 0) << "\n"
        << "quick_crush_press=" << d.crush_press << "\n"
        << "quick_crush_hold_ms=" << d.crush_hold_ms << "\n"
        << "quick_use_shake=" << (d.use_shake ? 1 : 0) << "\n"
        << "quick_shake_choir_bell=" << (d.shake_choir_bell ? 1 : 0) << "\n"
        << "quick_shake_strokes=" << d.shake_strokes << "\n"
        << "quick_shake_window_ms=" << d.shake_window_ms << "\n"
        << "quick_shake_wrist=" << d.shake_wrist << "\n"
        << "quick_shake_arm=" << d.shake_arm << "\n"
        << "quick_use_stab=" << (d.use_stab ? 1 : 0) << "\n"
        << "# the Tiny Music Box in the left hand frees the right hand: take its crank with the "
           "right "
           "grip and turn it to play (one turn); 0 = held to use with the game's animation\n"
        << "quick_music_box_crank=" << (d.music_box_crank ? 1 : 0) << "\n"
        << "music_box_grab_cm=" << d.music_box_grab_cm << "\n"
        << "music_box_turns=" << d.music_box_turns << "\n"
        << "music_box_replay_s=" << d.music_box_replay_s << "\n"
        << "# throwables fly with your hand's release velocity; 0 = the game's own throw\n"
        << "quick_throw=" << (d.throws ? 1 : 0) << "\n"
        << "# hand speed at release to throw (m/s; slower puts it back) and launch speed per hand "
           "speed (real ballistics; VR throws move the hand far slower than real ones)\n"
        << "throw_min_speed=" << d.throw_min_speed << "\n"
        << "throw_gain=" << d.throw_gain << "\n"
        << "# ... through this power curve about 3 m/s (1 = linear; below 1 a relaxed throw "
           "carries "
           "a little further, a hard one bends less into the cap)\n"
        << "throw_gain_curve=" << d.throw_gain_curve << "\n"
        << "# the release speed from the grip located at the input time (the freshest pose; 0 = "
           "the poses predicted for the display time)\n"
        << "throw_fresh_pose=" << (d.throw_fresh_pose ? 1 : 0) << "\n"
        << "# the grip opened while the hand still speeds up: the throw waits up to this long for "
           "its peak (ms, the item still in the hand; 0 = at the release)\n"
        << "throw_rise_wait_ms=" << d.throw_rise_wait_ms << "\n"
        << "# the hand's strongest velocity this long before the grip opens counts when it beats "
           "the release velocity (s; 0 = the release only), its direction at most this many "
           "degrees from the release's\n"
        << "throw_peak_window_s=" << d.throw_peak_window_s << "\n"
        << "throw_peak_turn_deg=" << d.throw_peak_turn_deg << "\n"
        << "# a throw from flat up to 15 degrees leaves this many degrees higher (fading out by 40 "
           "up; a throw aimed at something at eye level carries to it; 0 = your exact "
           "direction)\n"
        << "throw_lift_deg=" << d.throw_lift_deg << "\n"
        << "# a straight push gets this share of the gain (a wind-up, a wrist whip or an arm "
           "swung round: all of it)\n"
        << "throw_push_factor=" << d.throw_push_factor << "\n"
        << "# the release velocity is taken at the item: the controller's spin x the item's "
           "offset counts this much (0 = the grip's linear velocity only)\n"
        << "throw_spin=" << d.throw_spin << "\n"
        << "# soft cap of a throw: its arc at its own pitch lands at most this x as far as the "
           "game's own throw of the item (read from its bullet): a lob is held back, a flat throw "
           "may leave faster, up to the speed at the second (degrees; 45 = the best arc's speed "
           "at every pitch)\n"
        << "throw_range_factor=" << d.throw_range_factor << "\n"
        << "throw_cap_flat_deg=" << d.throw_cap_flat_deg << "\n"
        << "# the soft cap starts bending at this share of it (lower: a smoother approach)\n"
        << "throw_cap_knee=" << d.throw_cap_knee << "\n"
        << "# lock-on: the throw's yaw is turned this share toward the target when it is within "
           "this many degrees of it (knives: the second); 0 = off\n"
        << "throw_assist=" << d.throw_assist << "\n"
        << "throw_assist_deg=" << d.throw_assist_deg << "\n"
        << "throw_assist_knife_deg=" << d.throw_assist_knife_deg << "\n"
        << "# not locked on (every throw but the knives): the yaw turns this share toward the "
           "lock-on candidate nearest the throw's heading, within throw_assist_deg; 0 = lock-on "
           "only\n"
        << "throw_assist_unlocked=" << d.throw_assist_unlocked << "\n"
        << "# throwing / poison knives, locked on or not: the throw turns this share toward the "
           "direction that drops onto the target (the lock-on one, else the nearest to the "
           "throw's line) when within this many degrees of it, fading to none at 4/3 of them; "
           "0 = the yaw assist above\n"
        << "knife_assist=" << d.knife_assist << "\n"
        << "knife_assist_deg=" << d.knife_assist_deg << "\n"
        << "# the hunter's own horizontal velocity carried by a throw (0 = none)\n"
        << "throw_inherit_motion=" << d.throw_inherit_motion << "\n"
        << "# real gravity and no speed clamps for thrown items / gravity (m/s^2)\n"
        << "throw_physics=" << (d.throw_physics ? 1 : 0) << "\n"
        << "throw_gravity=" << d.throw_gravity << "\n"
        << "# the thrown item starts where it was in the hand (0 = at the palm, the game's spawn)\n"
        << "throw_from_hand=" << (d.throw_from_hand ? 1 : 0) << "\n"
        << "# the game's own throw whoosh as a bottle, knife or pebble leaves the hand / the "
           "release pulse scales with the launch speed (0 = the same for every throw)\n"
        << "throw_sound=" << (d.throw_sound ? 1 : 0) << "\n"
        << "throw_haptic_scale=" << (d.throw_haptic_scale ? 1 : 0) << "\n"
        << "# a Molotov in flight bursts in the air when your gun fires toward it (0 = off) / "
           "within this many degrees of the barrel's ray / the air burst's bullet (701 = the "
           "Cannon's explosion, 0 = the Molotov's own) and its hit radius (m, 0 = its own) / then its fire "
           "also lands on the ground "
           "below (0 = the air only)\n"
        << "molotov_burst=" << (d.molotov_burst ? 1 : 0) << "\n"
        << "molotov_burst_deg=" << d.molotov_burst_deg << "\n"
        << "molotov_burst_bullet=" << d.molotov_burst_bullet << "\n"
        << "molotov_burst_radius_m=" << d.molotov_burst_radius_m << "\n"
        << "molotov_burst_ground=" << (d.molotov_burst_ground ? 1 : 0) << "\n"
        << "# quick-item visuals drawn into the game's own frame (0 = none) and their brightness\n"
        << "inframe=" << (d.inframe ? 1 : 0) << "\n"
        << "inframe_exposure=" << d.inframe_exposure << "\n"
        << "# the held item and the wrist gauge move with the hunter's hand while you walk (0 = "
           "placed one game frame behind it, as before)\n"
        << "inframe_hand_sync=" << (d.inframe_hand_sync ? 1 : 0) << "\n";
}

} // namespace VR::Quick
