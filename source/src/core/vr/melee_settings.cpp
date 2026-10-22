// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <fmt/format.h>

#include "core/vr/melee_settings.h"

namespace VR::Melee {

namespace {

bool Flag(const std::string& value) {
    return value == "1" || value == "true";
}

const char* TelegraphText(int mode) {
    return mode == 0 ? "off" : mode == 2 ? "all" : "heavy";
}

// A melee_stamina_pct line was read: the old melee_stamina=1 no longer overrides it.
bool g_stamina_pct_given = false;

} // Anonymous namespace

MeleeSettings& GetMeleeSettings() {
    static MeleeSettings settings;
    return settings;
}

bool ParseMeleeSetting(const std::string& key, const std::string& value) {
    auto& s = GetMeleeSettings();
    const auto f = [&value] { return std::stof(value); };
    if (key == "melee") {
        if (value != "physical" && value != "vanilla") {
            throw std::invalid_argument{"physical or vanilla expected"};
        }
        s.physical = value == "physical";
    } else if (key == "melee_open") {
        if (value != "onset" && value != "commit") {
            throw std::invalid_argument{"onset or commit expected"};
        }
        s.open_onset = value == "onset";
    } else if (key == "melee_visceral") {
        s.visceral = Flag(value);
    } else if (key == "visceral_thrust") {
        s.visceral_thrust = Flag(value);
    } else if (key == "visceral_swing") {
        s.visceral_swing = Flag(value);
    } else if (key == "visceral_cue") {
        s.visceral_cue = std::clamp(std::stoi(value), 0, 2);
    } else if (key == "visceral_thrust_speed") {
        s.visceral_thrust_speed = std::clamp(f(), 0.8f, 6.0f);
    } else if (key == "visceral_thrust_cm") {
        s.visceral_thrust_cm = std::clamp(f(), 4.0f, 40.0f);
    } else if (key == "visceral_thrust_align_deg") {
        s.visceral_thrust_align_deg = std::clamp(f(), 10.0f, 60.0f);
    } else if (key == "visceral_pull") {
        s.visceral_pull = Flag(value);
    } else if (key == "visceral_pull_speed") {
        s.visceral_pull_speed = std::clamp(f(), 0.01f, 0.5f);
    } else if (key == "visceral_pull_lead_s") {
        s.visceral_pull_lead_s = std::clamp(f(), 0.0f, 0.5f);
    } else if (key == "visceral_pull_at_s") {
        s.visceral_pull_at_s = std::clamp(f(), 0.0f, 5.0f);
    } else if (key == "visceral_pull_timeout_s") {
        s.visceral_pull_timeout_s = std::clamp(f(), 0.5f, 10.0f);
    } else if (key == "visceral_pull_catch_s") {
        s.visceral_pull_catch_s = std::clamp(f(), 0.0f, 0.3f);
    } else if (key == "visceral_pull_entry_speed") {
        s.visceral_pull_entry_speed = std::clamp(f(), 1.0f, 1.5f);
    } else if (key == "visceral_pull_rip_speed") {
        s.visceral_pull_rip_speed = std::clamp(f(), 1.0f, 2.0f);
    } else if (key == "visceral_freehand") {
        s.visceral_freehand = Flag(value);
    } else if (key == "visceral_pull_recover_s") {
        s.visceral_pull_recover_s = std::clamp(f(), 0.1f, 1.0f);
    } else if (key == "visceral_pull_cm") {
        s.visceral_pull_cm = std::clamp(f(), 4.0f, 40.0f);
    } else if (key == "visceral_pull_mps") {
        s.visceral_pull_mps = std::clamp(f(), 0.3f, 4.0f);
    } else if (key == "visceral_pull_impact_ms") {
        s.visceral_pull_impact_ms = std::clamp(f(), 0.0f, 400.0f);
    } else if (key == "visceral_pull_learned") {
        s.visceral_pull_learned = value;
    } else if (key == "melee_speed_damage") {
        s.speed_damage = Flag(value);
    } else if (key == "melee_radius_scale") {
        s.radius_scale = std::clamp(f(), 0.3f, 1.0f);
    } else if (key == "melee_radius_fit") {
        s.radius_fit = Flag(value);
    } else if (key == "melee_radius_margin_cm") {
        s.radius_margin_cm = std::clamp(f(), 0.0f, 40.0f);
    } else if (key == "melee_haptics") {
        s.haptics = Flag(value);
    } else if (key == "melee_bite") {
        s.bite = Flag(value);
    } else if (key == "melee_whoosh") {
        s.whoosh = Flag(value);
    } else if (key == "melee_trail") {
        s.trail = Flag(value);
    } else if (key == "melee_charge_s") {
        s.charge_s = std::clamp(f(), 0.5f, 2.5f);
    } else if (key == "swing_buffer_ms") {
        s.swing_buffer_ms = std::clamp(f(), 0.0f, 400.0f);
    } else if (key == "melee_stamina_pct") {
        s.stamina_pct = std::clamp(std::stoi(value), 0, 100);
        g_stamina_pct_given = true;
    } else if (key == "melee_stamina") {
        // The old switch (until 2026-10-08): 1 = the game's full cost, kept for old vr.ini files
        // unless a melee_stamina_pct line says otherwise; 0 (the old default, which a written
        // vr.ini carries) leaves melee_stamina_pct as it is.
        if (Flag(value) && !g_stamina_pct_given) {
            s.stamina_pct = 100;
        }
    } else if (key == "melee_regen_pause") {
        s.regen_pause = Flag(value);
    } else if (key == "melee_telegraph") {
        if (value != "heavy" && value != "all" && value != "off") {
            throw std::invalid_argument{"heavy, all or off expected"};
        }
        s.telegraph = value == "off" ? 0 : value == "all" ? 2 : 1;
    } else if (key == "melee_telegraph_arc_deg") {
        s.telegraph_arc_deg = std::clamp(f(), 20.0f, 180.0f);
    } else if (key == "melee_telegraph_long_s") {
        s.telegraph_long_s = std::clamp(f(), 0.15f, 1.5f);
    } else if (key == "melee_budget") {
        s.budget = Flag(value);
    } else if (key == "melee_budget_charges") {
        s.budget_charges = std::clamp(std::stoi(value), 1, 8);
    } else if (key == "melee_budget_scale") {
        s.budget_scale = std::clamp(f(), 0.25f, 3.0f);
    } else if (key == "melee_budget_offset_ms") {
        s.budget_offset_ms = std::clamp(f(), -300.0f, 1000.0f);
    } else if (key == "melee_budget_quick_charges") {
        s.budget_quick_charges = std::clamp(std::stoi(value), 1, 8);
    } else if (key == "melee_budget_quick_s") {
        s.budget_quick_s = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "melee_plunge") {
        s.plunge = Flag(value);
    } else if (key == "melee_plunge_min_drop_m") {
        s.plunge_min_drop_m = std::clamp(f(), 0.3f, 10.0f);
    } else if (key == "melee_plunge_damage") {
        s.plunge_damage = std::clamp(f(), 1.0f, 4.0f);
    } else if (key == "melee_plunge_hold_s") {
        s.plunge_hold_s = std::clamp(f(), 0.5f, 6.0f);
    } else {
        return false;
    }
    return true;
}

void WriteMeleeDefaults(std::ostream& out) {
    const MeleeSettings d{};
    out << "# ---- Melee (the right controller) ----\n"
        << "# physical = the swing is the attack (no attack animation: the game's own attack on "
           "your blade while\n# it moves fast); vanilla = a swing presses R1 / R2 (the game's "
           "canned attack)\n"
        << "melee=" << (d.physical ? "physical" : "vanilla") << "\n"
        << "# onset = the hit window opens as the stroke starts (point-blank hits land); commit = "
           "only once the\n# swing is certain (about 0.1 s later)\n"
        << "melee_open=" << (d.open_onset ? "onset" : "commit") << "\n"
        << "# every swing first tries the game's visceral / riposte / backstab\n"
        << "melee_visceral=" << (d.visceral ? 1 : 0) << "\n"
        << "# physical viscerals: a THRUST of the right hand (fast, forward along the blade) "
           "starts the visceral\n# after a parry; a swing hits the staggered enemy normally "
           "(visceral_swing=1: a swing starts it too)\n"
        << "visceral_thrust=" << (d.visceral_thrust ? 1 : 0) << "\n"
        << "visceral_swing=" << (d.visceral_swing ? 1 : 0) << "\n"
        << "# a heartbeat on the right hand when a visceral opens: 0 off, 1 once, 2 every 1.1 s "
           "while it lasts\n"
        << "visceral_cue=" << d.visceral_cue << "\n"
        << "# the thrust: the hand's peak speed along the blade (m/s), its travel along it "
           "(cm) and how far\n# the motion may stray from the blade's line (deg)\n"
        << "visceral_thrust_speed=" << d.visceral_thrust_speed << "\n"
        << "visceral_thrust_cm=" << d.visceral_thrust_cm << "\n"
        << "visceral_thrust_align_deg=" << d.visceral_thrust_align_deg << "\n"
        << "# the interactive visceral: it plays in first person and waits just before the rip, "
           "slowed to\n# this speed with his hand inside; drawing your hand back out drives it "
           "to the rip, the yank\n# (cm, m/s) tears it out. The timeout rips at 80% damage. "
           "The first throw teaches timing.\n"
        << "visceral_pull=" << (d.visceral_pull ? 1 : 0) << "\n"
        << "# tracked hands throughout; just before the rip the world holds still until you "
           "pull (any time;\n# full damage). 0 restores the animated insertion and timed pull "
           "described above.\n"
        << "visceral_freehand=" << (d.visceral_freehand ? 1 : 0) << "\n"
        << "visceral_pull_speed=" << d.visceral_pull_speed << "\n"
        << "visceral_pull_timeout_s=" << d.visceral_pull_timeout_s << "\n"
        << "# a brief catch, faster entry and extraction; arms return this many clip seconds after damage\n"
        << "visceral_pull_catch_s=" << d.visceral_pull_catch_s << "\n"
        << "visceral_pull_entry_speed=" << d.visceral_pull_entry_speed << "\n"
        << "visceral_pull_rip_speed=" << d.visceral_pull_rip_speed << "\n"
        << "visceral_pull_recover_s=" << d.visceral_pull_recover_s << "\n"
        << "visceral_pull_cm=" << d.visceral_pull_cm << "\n"
        << "visceral_pull_mps=" << d.visceral_pull_mps << "\n"
        << "# the rip's slow-motion beat (ms; 0 = none)\n"
        << "visceral_pull_impact_ms=" << d.visceral_pull_impact_ms << "\n"
        << "# damage x 0.75 .. 1.10 by the blade's speed (with melee_budget=0 also by the swing "
           "rate, down to\n# 0.5; 0 = the game's own damage)\n"
        << "melee_speed_damage=" << (d.speed_damage ? 1 : 0) << "\n"
        << "# the game's hit capsule around the blade x this (0.3 .. 1; 1 = the game's own, "
           "which hits 40 cm\n# from the visible blade)\n"
        << "melee_radius_scale=" << d.radius_scale << "\n"
        << "# hit thump, wall clank, swing tick, charge buzz; the blade bites on a hit; the "
           "swing sound and trail\n"
        << "melee_haptics=" << (d.haptics ? 1 : 0) << "\n"
        << "melee_bite=" << (d.bite ? 1 : 0) << "\n"
        << "melee_whoosh=" << (d.whoosh ? 1 : 0) << "\n"
        << "melee_trail=" << (d.trail ? 1 : 0) << "\n"
        << "# right trigger held this long: the next swing is charged (0.35 s: heavy)\n"
        << "melee_charge_s=" << d.charge_s << "\n"
        << "# a swing begun just before a stagger hands the attack back waits this long (ms) for "
           "it, then\n# its attack opens on the swing under way; later = refused (the deny tick); "
           "0 = refused at once\n"
        << "swing_buffer_ms=" << d.swing_buffer_ms << "\n"
        << "# your light physical swings cost this percent of the game's stamina for the same attack "
           "(at least 1;\n# refused at 0 like the game's own); 0 = free, 100 = the game's full "
           "cost. Strong swings always cost 100%; insufficient stamina selects a light.\n# Dodges, sprint, jumps and the gun keep the game's\n"
        << "melee_stamina_pct=" << d.stamina_pct << "\n"
        << "# no stamina regen just after full-cost light or strong attacks ("
           "game recovery)\n"
        << "melee_regen_pause=" << (d.regen_pause ? 1 : 0) << "\n"
        << "# which swings the enemies can read and react to: heavy = a wind-up, a heavy, charged, "
           "long or wide\n# swing (a quick light flick never), all = every swing (as the game's "
           "own attacks), off = none\n"
        << "melee_telegraph=" << TelegraphText(d.telegraph) << "\n"
        << "# a wind-up counts when the blade sweeps this much away from the front (deg), a swing "
           "when it is\n# still moving after this long (s)\n"
        << "melee_telegraph_arc_deg=" << d.telegraph_arc_deg << "\n"
        << "melee_telegraph_long_s=" << d.telegraph_long_s << "\n"
        << "# the swing rate: each weapon has a few quick charges (a light swing uses one, a heavy "
           "two, a charged\n# one all); they come back at the weapon's own pace in the game (its "
           "recovery after that move,\n# the Saw Cleaver's light 0.57 s) x the scale + the offset "
           "(ms). A swing with none left does not\n# register (a dull buzz); transforms never use "
           "them. 0 = the old rule (each hit weaker the faster\n# you swing)\n"
        << "melee_budget=" << (d.budget ? 1 : 0) << "\n"
        << "melee_budget_charges=" << d.budget_charges << "\n"
        << "melee_budget_scale=" << d.budget_scale << "\n"
        << "melee_budget_offset_ms=" << d.budget_offset_ms << "\n"
        << "# quick weapons (a light swing recovering in this many seconds or less: the Saw "
           "Cleaver, the Blade of\n# Mercy, the Rakuyo's twin blades...) get this many charges, "
           "the third in a row a moment later and\n# slower back (no more than "
           "melee_budget_charges = off)\n"
        << "melee_budget_quick_charges=" << d.budget_quick_charges << "\n"
        << "melee_budget_quick_s=" << d.budget_quick_s << "\n"
        << "# the plunge: a swing while you fall keeps its attack until you land, so the blade "
           "hits what you\n# land on; after a fall this deep (m) its hits deal x the damage. "
           "No plunge animation\n"
        << "melee_plunge=" << (d.plunge ? 1 : 0) << "\n"
        << "melee_plunge_min_drop_m=" << d.plunge_min_drop_m << "\n"
        << "melee_plunge_damage=" << d.plunge_damage << "\n"
        << "melee_plunge_hold_s=" << d.plunge_hold_s << "\n";
}

std::string MeleeSettingsText() {
    const auto& s = GetMeleeSettings();
    return fmt::format("melee={} melee_open={} melee_visceral={} melee_speed_damage={} "
                       "melee_radius_scale={:.2f} melee_radius_fit={} melee_radius_margin_cm={:.0f} "
                       "melee_haptics={} melee_bite={} melee_whoosh={} "
                       "melee_trail={} melee_charge_s={:.2f} melee_stamina_pct={} "
                       "melee_regen_pause={} "
                       "melee_telegraph={} melee_telegraph_arc_deg={:.0f} "
                       "melee_telegraph_long_s={:.2f} melee_budget={} melee_budget_charges={} "
                       "melee_budget_scale={:.2f} melee_budget_offset_ms={:.0f} "
                       "visceral_thrust={} visceral_swing={} visceral_cue={} "
                       "visceral_thrust_speed={:.2f} visceral_thrust_cm={:.0f} "
                       "visceral_thrust_align_deg={:.0f} swing_buffer_ms={:.0f} "
                       "melee_plunge={} melee_plunge_min_drop_m={:.2f} melee_plunge_damage={:.2f} "
                       "melee_plunge_hold_s={:.2f}",
                       s.physical ? "physical" : "vanilla", s.open_onset ? "onset" : "commit",
                       s.visceral ? 1 : 0, s.speed_damage ? 1 : 0, s.radius_scale,
                       s.radius_fit ? 1 : 0, s.radius_margin_cm,
                       s.haptics ? 1 : 0, s.bite ? 1 : 0, s.whoosh ? 1 : 0, s.trail ? 1 : 0,
                       s.charge_s, s.stamina_pct, s.regen_pause ? 1 : 0,
                       TelegraphText(s.telegraph),
                       s.telegraph_arc_deg, s.telegraph_long_s, s.budget ? 1 : 0,
                       s.budget_charges, s.budget_scale, s.budget_offset_ms,
                       s.visceral_thrust ? 1 : 0, s.visceral_swing ? 1 : 0, s.visceral_cue,
                       s.visceral_thrust_speed, s.visceral_thrust_cm,
                       s.visceral_thrust_align_deg, s.swing_buffer_ms, s.plunge ? 1 : 0,
                       s.plunge_min_drop_m, s.plunge_damage, s.plunge_hold_s);
}

} // namespace VR::Melee
