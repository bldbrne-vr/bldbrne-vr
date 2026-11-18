// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>

#include "core/vr/two_hand_grip.h"

// The two-handed grip's vr.ini keys (core/vr/two_hand_grip.h). Parsed by VR::LoadSettings (keys it
// does not know go to ParseTwoHandSetting) and written with the defaults when vr.ini is created.
// Read live (the presenter reads them every frame).
namespace VR::TwoHand {

inline Settings& GetTwoHandSettings() {
    static Settings settings;
    return settings;
}

inline bool ParseTwoHandSetting(const std::string& key, const std::string& value) {
    auto& s = GetTwoHandSettings();
    const auto on = [&value] { return value == "1" || value == "true"; };
    if (key == "two_hand_grip") {
        s.enabled = on();
    } else if (key == "two_hand_guide") {
        s.guide = on();
    } else if (key == "two_hand_optimized_controls") {
        s.optimized_controls = on();
    } else if (key == "two_hand_trigger_hold") {
        s.trigger_hold = on();
    } else if (key == "two_hand_hold_b_reposition") {
        s.hold_b_reposition = true; // obsolete toggle option: old files always use hold-to-slide
    } else if (key == "two_hand_zone_cm") {
        s.zone_m = std::clamp(std::stof(value), 3.0f, 20.0f) * 0.01f;
    } else if (key == "two_hand_zone_seen") {
        s.zone_seen = on();
    } else if (key == "two_hand_release_cm") {
        s.release_m = std::clamp(std::stof(value), 8.0f, 60.0f) * 0.01f;
    } else if (key == "two_hand_twist_deg") {
        s.twist_deg = std::clamp(std::stof(value), 20.0f, 180.0f);
    } else if (key == "two_hand_damage") {
        s.damage = std::clamp(std::stof(value), 0.5f, 2.0f);
    } else if (key == "two_hand_damage_seen") {
        s.damage_seen = on();
    } else if (key == "two_hand_attach_ms") {
        s.attach_s = std::clamp(std::stof(value), 0.0f, 500.0f) * 0.001f;
    } else if (key == "two_hand_blend_ms") {
        s.blend_s = std::clamp(std::stof(value), 0.0f, 500.0f) * 0.001f;
    } else if (key == "two_hand_swing_speed") {
        s.swing_left_mps = std::clamp(std::stof(value), 0.0f, 5.0f);
    } else if (key == "two_hand_haptics") {
        s.haptics = on();
    } else if (key == "two_hand_swing_hum") {
        const float gain = std::stof(value);
        s.swing_hum = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.0f) : 0.0f;
    } else if (key == "two_hand_relative") {
        s.relative = on();
    } else {
        return false;
    }
    return true;
}

inline void WriteTwoHandDefaults(std::ostream& out) {
    const Settings d{};
    out << "# ---- Two-handed grip (a two-handed form: Ludwig's, Kirkhammer, Hunter Axe ... B) ----\n"
        << "# 1 = reach the left hand onto the weapon's handle and squeeze the left grip: both "
           "hands hold it\n# (the weapon points along your two hands); release both grip buttons "
           "to let go.\n# Elsewhere the left grip is unchanged (the item arc, the belt "
           "vial)\n"
        << "two_hand_grip=" << (d.enabled ? 1 : 0) << "\n"
        << "# faint off-hand target while holding the right grip (Weapons > Grip guide)\n"
        << "two_hand_guide=" << (d.guide ? 1 : 0) << "\n"
        << "# opt-in controls: stick transforms; left trigger grabs; each grip slides its hand\n"
        << "two_hand_optimized_controls=" << (d.optimized_controls ? 1 : 0) << "\n"
        << "# optimized scheme attachment: 1 hold left trigger (default), 0 press to toggle\n"
        << "two_hand_trigger_hold=" << (d.trigger_hold ? 1 : 0) << "\n"
        << "# two-handed grip mode; B threshold 160 ms, short taps keep their normal action\n"
        << "# B is hold-to-slide only; legacy toggle values are ignored\n"
        << "# the lead hand stays fixed; the other hand always slides; slide mode lets both slide\n"
        << "two_hand_hold_b_reposition=" << (d.hold_b_reposition ? 1 : 0) << "\n"
        << "# the handle zone's radius (cm): the left hand this close to the handle at the "
           "squeeze\n"
        << "two_hand_zone_cm=" << d.zone_m * 100.0f << "\n"
        << "two_hand_zone_seen=1\n"
        << "# legacy distance setting (cm); held grips no longer release on distance\n"
        << "two_hand_release_cm=" << d.release_m * 100.0f << "\n"
        << "# legacy angle setting (deg); held grips no longer release on twist\n"
        << "two_hand_twist_deg=" << d.twist_deg << "\n"
        << "# a two-handed swing's hits x this (both hands moving; 0.5 .. 2)\n"
        << "two_hand_damage=" << d.damage << "\n"
        << "two_hand_damage_seen=1\n"
        << "# the hunter's left hand onto / off the handle over this (ms)\n"
        << "two_hand_blend_ms=" << d.blend_s * 1000.0f << "\n"
        << "# fresh support-hand attachment only; never changes transformation duration\n"
        << "two_hand_attach_ms=" << d.attach_s * 1000.0f << "\n"
        << "# the left hand's speed (m/s) that makes a swing two-handed\n"
        << "two_hand_swing_speed=" << d.swing_left_mps << "\n"
        << "# the click on both hands at the grab, the soft tick at a release, the two-handed "
           "thump\n"
        << "two_hand_haptics=" << (d.haptics ? 1 : 0) << "\n"
        << "# faint fast-swing feedback on attached hands: 0 off .. 1 full (Weapons > Swing feel)\n"
        << "two_hand_swing_hum=" << d.swing_hum << "\n"
        << "# 1 = the weapon keeps the angle it had at the grab and the left hand steers it from "
           "there;\n# 0 = it snaps onto the line between your hands at the grab\n"
        << "two_hand_relative=" << (d.relative ? 1 : 0) << "\n";
}

} // namespace VR::TwoHand
