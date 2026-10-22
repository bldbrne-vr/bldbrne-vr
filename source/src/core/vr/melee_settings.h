// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <iosfwd>
#include <string>

// Physical melee (core/vr/melee_window.h, core/vr/bloodborne_melee.h): its vr.ini keys. Parsed by
// VR::LoadSettings, which hands every key it does not know to ParseMeleeSetting, and written with
// the other defaults when vr.ini is created. Read-only after loading; `melee` is decided at install
// (a restart changes it).
namespace VR::Melee {

struct MeleeSettings {
    // physical: the swing is the attack (no attack animation; the game's own attack on the real
    // blade while it moves fast); vanilla: a swing presses the game's R1 / R2 as before (its canned
    // attack animation). Weapons or forms without physical moves (bow and gun forms; the whips
    // with whip=canned) keep R1 / R2 either way.
    bool physical{true};
    // onset: the window opens as the stroke starts (point-blank hits land) and must pass the swing
    // gate within 0.15 s; commit: it opens only once the swing gate passed (about 80-100 ms later).
    bool open_onset{true};
    bool visceral{true}; // every swing first tries the game's visceral / riposte / backstab
    // Physical viscerals (core/vr/visceral_thrust.h): the visceral is a THRUST of the right hand
    // (fast, forward along the blade, the blade hardly turning), not any swing; a swing at a
    // staggered enemy hits it normally. visceral_swing=1: a swing starts it too (as before).
    bool visceral_thrust{true};
    bool visceral_swing{false};
    // The heartbeat on the right hand when a visceral opens (a target you hit staggers within
    // 6 m): 0 off, 1 once, 2 every 1.1 s while it stays open (3 at most).
    int visceral_cue{1};
    // (1.5 / 10 / 45: the feel review of 2026-10-06; 1.8 / 12 / 37 missed tentative, seated and
    // lunging stabs, core/vr/visceral_thrust.h Config)
    float visceral_thrust_speed{1.5f}; // m/s: the hand's peak speed along the blade
    float visceral_thrust_cm{10.0f};   // cm: the hand's travel along the blade
    float visceral_thrust_align_deg{45.0f}; // deg: the motion this close to the blade's line
    // The interactive visceral (core/vr/visceral_pull.h; 2026-10-12, not yet tried in the game):
    // viscerals, ripostes and backstabs play in first person (whatever cinematic_visceral says),
    // and just before the rip the hunter and his target slow to x visceral_pull_speed with his
    // hand inside it, the enemy's heartbeat in your right hand. Drawing your hand back out (any
    // way but further in) drives the throw toward the rip, and the yank that completes the pull
    // (visceral_pull_cm, at visceral_pull_mps) tears it out: the game's own rip, held for
    // visceral_pull_impact_ms. visceral_pull_timeout_s releases automatically, with 80% damage.
    // The first throw of each animation plays through at full damage and teaches its rip time
    // (kept in visceral_pull_learned); visceral_pull_at_s > 0 holds every throw at that clip time
    // instead (s, before the lead).
    bool visceral_pull{true};
    // Tracked arms; just before the rip the world holds still until you pull (from the catch
    // on, no deadline but a 20 s safety, no penalty; visceral_pull_timeout_s is legacy only).
    bool visceral_freehand{true};
    float visceral_pull_speed{0.04f};
    float visceral_pull_lead_s{0.10f};
    float visceral_pull_at_s{0.0f};
    float visceral_pull_timeout_s{3.0f};
    float visceral_pull_catch_s{0.10f}; // the catch lasts at least this long
    float visceral_pull_entry_speed{1.15f};
    float visceral_pull_rip_speed{1.4f};
    float visceral_pull_recover_s{0.25f}; // clip time after actual damage: visual arm recovery
    float visceral_pull_cm{12.0f};
    float visceral_pull_mps{0.9f}; // the yank's peak speed (m/s)
    float visceral_pull_impact_ms{150.0f}; // the rip's slow-motion beat (0: none)
    std::string visceral_pull_learned{}; // "clip:seconds,..." (written by the game)
    // Damage x [0.75, 1.10] by the blade tip's speed (no full damage from a gentle tap) and, with
    // melee_budget=0, the swing cadence (down to 0.5: no more than the game's own damage per
    // second from flailing); 0 = the row's.
    bool speed_damage{true};
    // The game's attack capsule radius x this (its 0.3-0.5 m hit 40 cm from the visible blade).
    float radius_scale{0.7f};
    // ... and never more than the blade's own reach off the capsule axis (from the weapon's model)
    // + this margin (a thin sword 0.12 m instead of 0.28; core/vr/melee_window.h SwingRadius).
    bool radius_fit{true};
    float radius_margin_cm{10.0f};
    bool haptics{true};     // the hit thump, the wall clank, the window tick, the charge buzz
    bool bite{true};        // the blade bites: the hand lags the controller for a moment on a hit
    bool whoosh{true};      // the weapon's swing sound at the commit
    bool trail{true};       // the blade trail effect while the window is open
    float charge_s{1.0f};   // right trigger held this long: a charged swing (0.35 s: heavy)
    // A swing begun while a kept stagger (or, with dodge_attack=0, a dodge) has not yet handed
    // the attack back waits this long for the game's attack window, then opens its attack on the
    // swing under way (core/vr/melee_window.h SwingBuffer); later: refused (the deny tick). 0 =
    // refused at once (as before).
    float swing_buffer_ms{150.0f};
    // melee_stamina_pct (core/vr/melee_window.h SwingStaminaOf, PartialSwing): the hunter's
    // light physical swings (including form B and the physical whip) cost this percent of the game's own cost for the same attack: the game takes its row's
    // stamina and all but this share goes back in the same call (the gauge dips by the share
    // only; at least 1 of a cost above 0), a swing at 0 or below is refused as the game's own, no
    // regen pause. Heavy/charged attacks always pay 100%; insufficient stamina selects a light.
    // The default is 80%. 100 = the game's cost (melee_regen_pause too);
    // 0 = free (the game's no-stamina-consumption flag held for the call, never refused). Dodges,
    // sprint, jumps, the gun and the game's own canned attacks (melee=vanilla, R1 / R2 forms) keep
    // the game's stamina. The old melee_stamina=1 still means 100 (a melee_stamina_pct line wins);
    // melee_stamina=0, the old default every written vr.ini carries, leaves this as it is.
    int stamina_pct{80};
    bool regen_pause{true}; // no stamina regen just after a swing (as the game's own attack;
                            // full-cost light attacks and all heavy/charged attacks)
    // Which swings the enemies may read (core/vr/melee_window.h TelegraphTracker): 1 = heavy (a
    // wind-up, a heavy, charged, long or wide swing sends the game's own "the target attacks"
    // notice to the AIs, as a vanilla attack does; a quick light flick never does), 2 = all (every
    // swing, as vanilla), 0 = off. VR::Melee::TelegraphMode.
    int telegraph{1};
    float telegraph_arc_deg{60.0f}; // a wind-up stroke sweeping this much tells (deg)
    float telegraph_long_s{0.40f};  // a swing whose window is still open after this long tells
    // The combo budget (core/vr/melee_window.h ComboBudget): each weapon has `budget_charges`
    // quick charges; a light swing uses one, a heavy two, a charged one all, and they come back
    // after the move's own recovery in the game (its TAE's R1 cancel, core/vr/melee_moves.h
    // Paces) x budget_scale + budget_offset_ms. A swing with too few left does not register (no
    // attack, a dull buzz). Transforms never use charges. Off: the old rules (one swing per
    // 0.35 s at most, each hit weaker the faster you swing).
    bool budget{true};
    int budget_charges{2};
    float budget_scale{1.0f};
    // The tester's -20 (2026-10-05), -50 (10-06), then 60 ms shorter than -20: -80 (10-07).
    float budget_offset_ms{-80.0f};
    // Quick weapons (a light recovery of budget_quick_s or less in the form): this many charges,
    // the third a moment later and slower back (core/vr/melee_window.h BudgetConfig; the tester,
    // 2026-10-15). 2: off.
    int budget_quick_charges{3};
    float budget_quick_s{0.64f};
    // The physical plunge (core/vr/plunge.h; 2026-10-12, not yet tried in the game): a swing
    // while the hunter falls keeps its attack until he lands (+0.15 s), so the blade you hold out
    // hits what you land on; after a fall at least melee_plunge_min_drop_m deep its hits deal
    // x melee_plunge_damage. No plunge animation (R1 is never pressed). Needs dodge_camera=1 (the
    // landed byte and height are read with its signals).
    bool plunge{true};
    float plunge_min_drop_m{1.2f};
    float plunge_damage{1.35f}; // (1.5 until 2026-10-16: the tester)
    float plunge_hold_s{2.5f}; // the attack never held longer than this after it opened
};

MeleeSettings& GetMeleeSettings();

// VR::LoadSettings: true if `key` is one of ours (and it was applied).
bool ParseMeleeSetting(const std::string& key, const std::string& value);
// The defaults block written into a new vr.ini.
void WriteMeleeDefaults(std::ostream& out);
// "melee=physical melee_open=onset ..." for the install line.
std::string MeleeSettingsText();

} // namespace VR::Melee
