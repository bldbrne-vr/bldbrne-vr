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
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "core/vr/arm_ik.h"

// Trick weapons played by hand (2026-10-12, not yet tried in the game).
//
// hand_transform (2026-10-12): what
// the right grip does depends on the weapon (GripUseOf, RightGrip):
//  - the Chikage, Hunter Axe, Burial Blade, Kirkhammer, Ludwig's Holy Blade and Rifle Spear: the
//    right grip frees the left hand; the left grip on the handle (core/vr/two_hand_grip.h) turns
//    it into its two-handed form, which lasts only while both hands hold it (HoldForm);
//  - the Blade of Mercy and Rakuyo: the right grip frees the left hand; take the hilt with it
//    (squeeze the left grip next to the right hand) and pull the hands apart: the blade splits
//    in two; bring the two blades together, side by side, and squeeze the left grip: they lock
//    back into one;
//  - the Bloodletter: hold the right grip, turn it round and drive it into your chest (its L1);
//    the right grip again turns it back;
//  - Tonitrus: hold the right grip and lift the weapon up over your head: it charges (its L1);
//    then a cooldown;
//  - the Boom Hammer: its furnace starts like an engine (boom_pull): squeeze the right grip, a
//    pull, until it catches (3 to 5 pulls); it idles, rumbling: keep the grip held and it revs
//    up and lights, squeeze it again and again (each pumps it up), or strike its head: it lights.
//    Before it catches, a strike within 0.28 s of a starter CHUFF lights it early;
//  - the Beast Claw: hold it 2 s; Simon's Bowblade: hold it 0.5 s to open the bow (a squeeze
//    while fighting with the blade never does; the bow closes with a squeeze);
//  - the Whirligig Saw: right grip does nothing; the left hand grabbing the handle transforms it.
//  - the others: a squeeze transforms, as before.
// shield_raise: the Wooden Shield or the Loch Shield in the left hand blocks while you hold it up
// in front of you (the game's L2 held).
// gun_blade / moon_wave / stake_blast / weapon_hum / tool_aim: core/vr/bloodborne_gun.cpp (the
// right hand's shots, the Augur's aim) and the presenter (the trigger, the swing, the haptics);
// their data is here.
//
// Pure (no emulator dependencies; tests/test_trick_weapons.cpp). OpenXR LOCAL space (+x right,
// +y up, +z back, metres).
namespace VR::Tricks {

using Body::Vec3;

constexpr std::int32_t FistFamily = 10;

struct Settings {
    bool hand_transform{true}; // the right grip's uses above (0: a squeeze transforms them all)
    bool shield_raise{true};   // a shield held up blocks
    bool shield_guard{true};   // the shields guard better (core/vr/shield_guard.h)
    bool gun_blade{true};      // the transformed Reiterpallasch's shot (the trigger), the Rifle
                               // Spear's blast (the trigger held, let go)
    bool moon_wave{true};      // the Holy Moonlight Sword's wave on a charged swing
    bool stake_blast{true};    // the Stake Driver's blast (the trigger held, let go)
    bool weapon_hum{true};     // the Whirligig Saw's and Boom Hammer's hum, revving with a swing
    bool kos_visions{true};    // the Kos ritual's vision (KosRitualAt; vr_kos_ritual.glsl)
    bool saw_spin{true};       // the transformed Whirligig Saw held in both hands: left trigger
                               // spins it (right in the optimized scheme) (SawSpin)
    bool tool_aim{true};       // the tools that fire something fly where you aim (ToolAims)
    bool melee_left{true};     // a left-hand swing attacks with the left hand's blade, claw, fist
    bool weapon_wall{true};    // solid weapons: they stop at walls and each other, thud, buzz,
                               // spark (core/vr/solid_weapon.h, weapon_wall.h); off: none of it
                               // runs
    bool weapon_wall_map{true}; // ... at the walls too (weapon_wall=1, the default since 0.7.0, the
                               // user 2026-10-24); weapon_wall=2: weapons only, solid against each
                               // other but through the walls, as in the game
    bool weapon_wall_shape{true}; // ... the right weapon's shape against the other weapon fitted
                                  // to its own model (core/vr/weapon_fit.h, 2026-10-25; the walls
                                  // meet the line); 0: the line along its blade as in 0.7.0
    bool haptics{true};
    float pull_m{0.15f};   // a split: the hands this much further apart than at the grab
                           // (18 cm until the tester, 2026-11-10: "15cm")
    bool pull_seen{false}; // hand_transform_pull_seen: the 18 -> 15 cm move ran (vr_session.cpp)
    float shield_m{0.18f}; // the left hand this far in front of the eyes: the shield up
    float charge_cooldown_s{8.5f}; // Tonitrus' charge, the Boom Hammer's ignition: then this long
                                   // (2026-11-10; 10 before)
    bool cooldown_seen{false}; // charge_cooldown_seen: the 10 -> 8.5 s move ran (vr_session.cpp)
    // boom_pull: the Boom Hammer's furnace started like an engine (RightGrip, GripUse::Pull; the
    // user, 2026-11-06: "press 1-3 times and when it starts vibrate to either keep it held, or
    // smack it"): each right-grip squeeze is a pull, coughing until it catches (on a pull between
    // boom_pulls_min and boom_pulls_max, drawn each time; pulls further apart than
    // boom_pull_gap_s start over); caught it idles boom_idle_s, held it revs up over boom_rev_s
    // and lights (the tester: "make that hold longer"), each squeeze more pumps it up boom_pump
    // ("if we press grip many times in succession ... it should still light up"), struck it
    // lights at once. Before the catch, each squeeze opens BoomStrikeWindowS for a timed strike;
    // a late strike does not count as another pull or open a fresh window. Then boom_cooldown_s.
    // 0: held 1.5 s, as before.
    bool boom_pull{true};
    // 3 to 5 (2026-11-06: "at least 3 (with max up to 5)"; 2 to 4 tried, then "actually
    // 3-5 felt good"; 1 to 3 first).
    int boom_pulls_min{3}, boom_pulls_max{5};
    float boom_pull_gap_s{1.2f};
    float boom_idle_s{2.0f};
    float boom_rev_s{2.1f};
    float boom_pump{0.3f};
    float boom_cooldown_s{3.0f};
    float boom_volume{1.0f}; // its cough and growl (0: silent)
    // The Boom Hammer's fire goes out after this many hits landed while lit, once that swing is
    // over (the base game's buff is one hit; the tester, 2026-11-06: "can we have the fire go out
    // after doing a hit? right now it feels like it lasts forever"). 0: it stays lit.
    int boom_fire_hits{1};
    // melee_left_rows: the left hand's attack row per weapon, learned in the game (LeftRowOf)
    std::string left_rows{};
    // l2_canned: the right weapons (vr.ini weapon_grip_<key> keys, comma separated) whose
    // two-handed form still sends the left trigger to the game as L2 (its canned special, with
    // the animation); in every other two-handed form the left trigger does nothing.
    std::string l2_canned{"bloodletter,kos_parasite"};
};

inline Settings& GetTrickSettings() {
    static Settings settings;
    return settings;
}

inline bool ParseTrickSetting(const std::string& key, const std::string& value) {
    auto& s = GetTrickSettings();
    const bool on = value == "1" || value == "true";
    if (key == "hand_transform") {
        s.hand_transform = on;
    } else if (key == "shield_raise") {
        s.shield_raise = on;
    } else if (key == "shield_guard") {
        s.shield_guard = on;
    } else if (key == "gun_blade") {
        s.gun_blade = on;
    } else if (key == "moon_wave") {
        s.moon_wave = on;
    } else if (key == "stake_blast") {
        s.stake_blast = on;
    } else if (key == "weapon_hum") {
        s.weapon_hum = on;
    } else if (key == "saw_spin") {
        s.saw_spin = on;
    } else if (key == "kos_visions") {
        s.kos_visions = on;
    } else if (key == "tool_aim") {
        s.tool_aim = on;
    } else if (key == "melee_left") {
        s.melee_left = on;
    } else if (key == "weapon_wall") {
        s.weapon_wall = on || value == "2";
        s.weapon_wall_map = value != "2";
    } else if (key == "weapon_wall_shape") {
        s.weapon_wall_shape = on;
    } else if (key == "trick_haptics") {
        s.haptics = on;
    } else if (key == "hand_transform_pull_cm") {
        s.pull_m = std::clamp(std::stof(value), 8.0f, 40.0f) * 0.01f;
    } else if (key == "hand_transform_pull_seen") {
        s.pull_seen = on;
    } else if (key == "charge_cooldown_seen") {
        s.cooldown_seen = on;
    } else if (key == "shield_raise_cm") {
        s.shield_m = std::clamp(std::stof(value), 10.0f, 45.0f) * 0.01f;
    } else if (key == "charge_cooldown_s") {
        s.charge_cooldown_s = std::clamp(std::stof(value), 0.0f, 120.0f);
    } else if (key == "boom_pull") {
        s.boom_pull = on;
    } else if (key == "boom_pulls_min") {
        s.boom_pulls_min = std::clamp(static_cast<int>(std::lround(std::stof(value))), 1, 6);
    } else if (key == "boom_pulls_max") {
        s.boom_pulls_max = std::clamp(static_cast<int>(std::lround(std::stof(value))), 1, 6);
    } else if (key == "boom_pull_gap_s") {
        s.boom_pull_gap_s = std::clamp(std::stof(value), 0.3f, 5.0f);
    } else if (key == "boom_idle_s") {
        s.boom_idle_s = std::clamp(std::stof(value), 0.5f, 10.0f);
    } else if (key == "boom_rev_s") {
        s.boom_rev_s = std::clamp(std::stof(value), 0.2f, 5.0f);
    } else if (key == "boom_pump") {
        s.boom_pump = std::clamp(std::stof(value), 0.0f, 1.0f);
    } else if (key == "boom_cooldown_s") {
        s.boom_cooldown_s = std::clamp(std::stof(value), 0.0f, 120.0f);
    } else if (key == "boom_volume") {
        s.boom_volume = std::clamp(std::stof(value), 0.0f, 2.0f);
    } else if (key == "boom_fire_hits") {
        s.boom_fire_hits = std::clamp(static_cast<int>(std::lround(std::stof(value))), 0, 10);
    } else if (key == "melee_left_rows") {
        s.left_rows = value;
    } else if (key == "l2_canned") {
        s.l2_canned = value;
    } else {
        return false;
    }
    return true;
}

inline void WriteTrickDefaults(std::ostream& out) {
    const Settings d{};
    const auto b = [](bool v) { return v ? 1 : 0; };
    out << "# ---- Trick weapons played by hand ----\n"
        << "# 1 = transform by hand: for the Chikage, Hunter Axe, Burial Blade, Kirkhammer, "
           "Ludwig's and Rifle\n# Spear the right grip frees the left hand, the left grip on the "
           "handle transforms it, letting\n# go turns it back; the Blade of Mercy / Rakuyo: the "
           "right grip, then pull them apart; the\n# Bloodletter: hold the right grip, stab it "
           "into your chest; Tonitrus: hold the right grip\n# and lift it up; the Boom Hammer, the "
           "Beast Claw and Simon's Bowblade (into the bow): hold the right\n# grip (the Boom "
           "Hammer: squeeze it until its furnace catches, then hold it or strike the head:\n# "
           "boom_pull below) (0: a squeeze transforms them all)\n"
        << "# Whirligig Saw: right grip does nothing; the left-hand grab transforms it.\n"
        << "hand_transform=" << b(d.hand_transform) << "\n"
        << "# a split: the hands this much further apart (cm) than where they took hold\n"
        << "hand_transform_pull_cm=" << d.pull_m * 100.0f << "\n"
        << "hand_transform_pull_seen=1\n"
        << "# Tonitrus' charge and the Boom Hammer's ignition: none again for this long (s)\n"
        << "charge_cooldown_s=" << d.charge_cooldown_s << "\n"
        << "charge_cooldown_seen=1\n"
        << "# 1 = the Boom Hammer's furnace starts like an engine: each right-grip squeeze is a "
           "pull that\n# coughs until it catches (on a pull between boom_pulls_min and "
           "boom_pulls_max); then it idles,\n# rumbling, boom_idle_s: keep the grip held and it "
           "revs up over boom_rev_s and lights, squeeze\n# again and each squeeze pumps it up "
           "boom_pump (0..1 of the way), or strike its head with your\n# left hand, gun or "
           "torch: it lights at once. Before it catches, hit within 0.28 s of a starter CHUFF\n# "
           "to light it early. Then boom_cooldown_s before the next (0: hold the grip 1.5 s,\n# "
           "then charge_cooldown_s)\n"
        << "boom_pull=" << b(d.boom_pull) << "\n"
        << "boom_pulls_min=" << d.boom_pulls_min << "\n"
        << "boom_pulls_max=" << d.boom_pulls_max << "\n"
        << "# pulls further apart than this (s) start over\n"
        << "boom_pull_gap_s=" << d.boom_pull_gap_s << "\n"
        << "boom_idle_s=" << d.boom_idle_s << "\n"
        << "boom_rev_s=" << d.boom_rev_s << "\n"
        << "boom_pump=" << d.boom_pump << "\n"
        << "boom_cooldown_s=" << d.boom_cooldown_s << "\n"
        << "# its cough and growl (the game's own sounds; 0 = silent)\n"
        << "boom_volume=" << d.boom_volume << "\n"
        << "# the Boom Hammer's fire goes out after this many hits landed while lit, once that "
           "swing is over\n# (the base game: one; 0 = it stays lit)\n"
        << "boom_fire_hits=" << d.boom_fire_hits << "\n"
        << "# the right weapons whose two-handed form keeps the game's own left-trigger special "
           "(L2, with its\n# animation), by their weapon_grip_ keys; any other two-handed form's "
           "left trigger does nothing\n"
        << "l2_canned=" << d.l2_canned << "\n"
        << "# 1 = a shield held up in front of you blocks (the left hand this far in front of "
           "your eyes, cm)\n"
        << "shield_raise=" << b(d.shield_raise) << "\n"
        << "shield_raise_cm=" << d.shield_m * 100.0f << "\n"
        << "# 1 = the Wooden and Loch Shields guard better: physical 75% / 55% blocked, stability "
           "35 (hits\n# still get through, blocks still cost stamina); 0 = the game's own values\n"
        << "shield_guard=" << b(d.shield_guard) << "\n"
        << "# 1 = the transformed Reiterpallasch shoots at the right trigger along the blade; "
           "the transformed\n# Rifle Spear blasts: hold the trigger 0.3 s to charge it, let go\n"
        << "gun_blade=" << b(d.gun_blade) << "\n"
        << "# 1 = a charged swing of the transformed Holy Moonlight Sword sends its wave as the "
           "blade crosses\n# where you face (your heading, the sword's up/down), or when you let "
           "go of the trigger\n# mid-swing (along the blade); at the target in front of you "
           "either way\n"
        << "moon_wave=" << b(d.moon_wave) << "\n"
        << "# 1 = the transformed Stake Driver: hold the trigger 1 s, punch while held or release "
           "to discharge\n"
        << "stake_blast=" << b(d.stake_blast) << "\n"
        << "# 1 = the transformed Whirligig Saw and Boom Hammer hum faintly in your hand and rev "
           "up as you swing\n"
        << "weapon_hum=" << b(d.weapon_hum) << "\n"
        << "# 1 = the transformed Whirligig Saw held with both hands (the left grip on its "
           "handle): left trigger\n# (right in the optimized scheme) held spins it up, let go it "
           "spins down (never the "
           "game's canned special)\n"
        << "saw_spin=" << b(d.saw_spin) << "\n"
        << "# 1 = the Kos Parasite's ritual (the left trigger held in form B, with the Milkweed "
           "rune) folds\n# the edge of your view into a kaleidoscope of flesh and eyes until its "
           "burst (the centre stays\n# clear); 0 = the game's ritual alone\n"
        << "kos_visions=" << b(d.kos_visions) << "\n"
        << "# 1 = the hunter tools that fire something fly where you aim: the Augur, the "
           "Blacksky Eye and the\n# Accursed Brew where you look when your left hand pointed "
           "there, else where it points;\n# Tiny Tonitrus' lightning runs where you look\n"
        << "tool_aim=" << b(d.tool_aim) << "\n"
        << "# 1 = a left-hand swing attacks too: the second blade of the transformed Blade of "
           "Mercy and\n# Rakuyo, the Beast Claw's left claw, the Kos Parasite, the Torch, the Fist "
           "of Gratia, the bare\n# fist, a punch with a pistol in the hand (both hands swing at "
           "one pace)\n"
        << "melee_left=" << b(d.melee_left) << "\n"
        << "# 1 = solid weapons: your weapons and hands stop at walls and at each other; a strike "
           "thuds, a\n# scrape buzzes and throws a few sparks (0: they pass through, as in the "
           "game; 2: weapons\n# only: solid against each other, through the walls)\n"
        << "weapon_wall="
        << (!d.weapon_wall      ? 0
            : d.weapon_wall_map ? 1
                                : 2)
        << "\n"
        << "# 1 = the right weapon meets your other weapon with a shape fitted to its own model "
           "(read from\n# your game files: the folded Saw Cleaver's blade, an axe's head, the "
           "scythe; the walls meet a\n# line along its blade); 0 = a line along its blade\n"
        << "weapon_wall_shape=" << b(d.weapon_wall_shape) << "\n"
        << "trick_haptics=" << b(d.haptics) << "\n";
}

// ---- The weapons ---------------------------------------------------------------------------------

// How a right weapon (its family: EquipParamWeapon id / 100000) transforms by hand (HandTransform's
// gestures).
enum class Kind : std::uint8_t {
    None,
    DualBlades,  // the Blade of Mercy (40), Rakuyo (270): split / join
    Bloodletter, // 290: the stab (its L1)
};

constexpr Kind KindOf(std::int32_t family) {
    switch (family) {
    case 40:
    case 270:
        return Kind::DualBlades;
    case 290:
        return Kind::Bloodletter;
    default:
        return Kind::None;
    }
}

// The game transforms these with L1 and its own animation (the buff, the blood coating, the
// self-damage are in it): Tonitrus (130), the Chikage (20), the Bloodletter (290); the others
// with the weapon-only transform.
constexpr bool UsesL1(std::int32_t family) {
    return family == 130 || family == 20 || family == 290;
}

// Form B only while both hands hold the handle (HoldForm): the Chikage (20), the Hunter Axe (50),
// the Burial Blade (51), the Kirkhammer (80), Ludwig's Holy Blade (81), the Rifle Spear (100),
// Logarius' Wheel (120: its second handle, across the wheel; the tester, 2026-11-06) and the
// Whirligig Saw (310; its grab does not need a right-grip squeeze first).
constexpr bool GrabFamily(std::int32_t family) {
    return family == 20 || family == 50 || family == 51 || family == 80 || family == 81 ||
           family == 100 || family == 120 || family == 310;
}

// What a right-grip squeeze does (RightGrip).
enum class GripUse : std::uint8_t {
    None,      // bare fists; the saw changes form only through its off-hand grab
    Transform, // the squeeze transforms it (the right grip as it always was)
    Free,      // frees the left hand: its grab on the handle (GrabFamily) or the hilt (the dual
               // blades' split) transforms it
    Arm,       // arms the Bloodletter's stab
    Lift,      // held, the weapon lifted up over the head: Tonitrus' charge (its L1)
    Hold,      // held HoldSOf: the Beast Claw's unleashing, the Bowblade's bow (the Boom Hammer's
               // ignition with boom_pull=0)
    Pull,      // the Boom Hammer's furnace started like an engine (Settings::boom_pull)
};

constexpr GripUse GripUseOf(std::int32_t family, bool form_b) {
    if (family == 310 || family == FistFamily) {
        return GripUse::None;
    }
    if (family == 130) {
        return GripUse::Lift; // (one form: its charge is a buff)
    }
    if (form_b) {
        return GripUse::Transform;
    }
    if (GrabFamily(family) || family == 40 || family == 270) {
        return GripUse::Free;
    }
    if (family == 290) {
        return GripUse::Arm;
    }
    if (family == 280) {
        return GripUse::Pull;
    }
    if (family == 90 || family == 320) {
        return GripUse::Hold;
    }
    return GripUse::Transform;
}

// How long the grip is held (s): the Beast Claw's 2 s, the Boom Hammer's ignition, the Bowblade's
// bow (0.5 s: only no accidental squeeze).
constexpr float HoldSOf(std::int32_t family) {
    return family == 90 ? 2.0f : family == 320 ? 0.5f : 1.5f;
}

// Whether a charge (Lift, Hold) has the cooldown after it: Tonitrus' and the Boom Hammer's.
constexpr bool CoolsDown(std::int32_t family) {
    return family == 130 || family == 280;
}

// Whether `key` is one of the comma-separated `list` (l2_canned; spaces round an item ignored).
constexpr bool InKeyList(std::string_view list, std::string_view key) {
    while (!key.empty() && !list.empty()) {
        const std::size_t comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (item == key) {
            return true;
        }
    }
    return false;
}

// The right hand's shots (core/vr/bloodborne_gun.cpp): fired by the game's own bullet call from
// the weapon's behaviour rows, with no animation.
enum class Shot : std::uint8_t { None, GunBlade, MoonWave, StakeBlast, Bow, KosSpit };

struct ShotWeapon {
    std::int32_t variation; // the right weapon's behaviour variation
    Shot shot;
    float cadence_s;        // shot to shot (GunBlade, StakeBlast: a blast's cooldown)
    float charge_s;         // the trigger held this long, let go: the blast (0: the squeeze fires)
    std::int32_t flash_ffx; // the muzzle flash at `tip` (-1: none)
    std::int32_t bang;      // the shot's sound (-1: none)
    std::int32_t dry;       // ... without bullets
    std::int32_t base;      // first transformed-part marker (not necessarily behind the muzzle)
    std::int32_t tip;       // second marker; the gun-blades' muzzle/flash and native shot dummy
    bool reverse_line{false}; // reverse direction only; keep the native emission/flash markers
};

// The pistol's flash and sounds for the Reiterpallasch, the rifle's for the Rifle Spear
// (core/vr/gun_aim.h DirectGunOf): their own are in their shot clips, which do not play. The Rifle
// Spear charges in RifleChargeS, the Stake Driver in BlastChargeS with the trigger held;
// Projectile blasts take a bullet each; RifleCooldownS / BlastCooldownS after firing.
constexpr float RifleChargeS = 0.3f;
constexpr float RifleCooldownS = 1.0f;
constexpr float BlastChargeS = 1.0f;
constexpr float BlastCooldownS = 3.0f;

// The Kos Parasite's spit (core/vr/kos_lash.h; the tester, 2026-11-09: "use the game's own poison
// spit"): with the Milkweed rune (its tendril weapon, variation 3800) the left trigger in form A,
// a tap in form B, spits the game's own glob with no animation: its spit clips' (a156_032500,
// 042500) TAE event 2, the behaviour judge 200 (bullet 960, 6 m/s; on a hit its 0.8 m cloud 961,
// SpEffect 6190: poison) from the mouth (dummy 7), here from your mouth where you look,
// KosSpitLiftDeg above it as the glob arcs. The clip's sounds (SoundType 1: the gag 8000, 950, 951,
// 923) and its effect at the mouth (250102, 0.67 s; third person only: at your eyes it filled the
// view). Not in ShotWeaponOf: that table is the right trigger's.
constexpr std::int32_t KosRuneVariation = 3800;
constexpr bool KosFamily(std::int32_t family) {
    return family == 380 || family == 400 || family == 410;
}
constexpr bool CannedSpecialAvailable(std::int32_t family, std::int32_t variation) {
    // The no-Milkweed Parasite has fists, not the ritual. Its trigger must not
    // steal a physical heavy punch and turn it into the game's L2 animation.
    return !KosFamily(family) || variation == KosRuneVariation;
}
constexpr std::int32_t KosSpitJudge = 200;
constexpr std::int32_t KosSpitMouth = 7;    // the hunter's dummy at the front of his face
constexpr std::int32_t KosSpitFfx = 250102; // the glob gathering at the mouth
constexpr float KosSpitFfxS = 0.67f;
constexpr float KosSpitCooldownS = 0.9f; // the game's spit clip's pace
constexpr float KosSpitLiftDeg = 12.0f;
// The glob starts this far ahead of the mouth, and below it (the chin): nearer, in first person the
// game's glob covered the whole view for half a second (MockXR 2026-11-09).
constexpr float KosSpitAheadM = 0.30f;
constexpr float KosSpitChinM = 0.06f;
constexpr std::array<std::int32_t, 4> KosSpitSounds{8000, 951, 950, 923};
constexpr float KosRitualHoldS = 0.25f; // form B: held this long, the left trigger is the ritual
// The ritual in your hands (the user's mock page, 2026-11-09): a heartbeat quickening through the
// arms' rise and the coil (its beats, s after the ritual starts), then one deep pulse at its burst
// (the game's ritual a156_046000: the burst at frame 42, 1.40 s).
constexpr std::array<float, 5> KosRitualBeatsS{0.0f, 0.42f, 0.78f, 1.06f, 1.27f};
constexpr float KosRitualBurstS = 1.40f;
// Its vision (kos_visions; host_shaders/vr_kos_ritual.glsl) at `age` s after it began: the rise
// eases in through the arms' rise and the coil (0 to 1 by the burst), then the burst flashes and
// eases out over KosRitualSettleS (the eyes shut, a ring of light rushes out). Nothing before or
// after.
constexpr float KosRitualSettleS = 0.6f;
struct KosRitualLook {
    float rise{0.0f};
    float burst{0.0f};
};
inline KosRitualLook KosRitualAt(float age) {
    KosRitualLook l{};
    if (!(age >= 0.0f) || age >= KosRitualBurstS + KosRitualSettleS) {
        return l;
    }
    if (age < KosRitualBurstS) {
        const float u = age / KosRitualBurstS;
        l.rise = u * u * (3.0f - 2.0f * u);
    } else {
        const float v = (age - KosRitualBurstS) / KosRitualSettleS;
        l.burst = 1.0f - v * v * (3.0f - 2.0f * v);
    }
    return l;
}
// Confirmed animation start, shared by native and reconstructed eyes.
inline std::array<float, 4> KosRitualVisionAt(std::int64_t now, std::int64_t start) {
    if (start == 0 || now < start) {
        return {};
    }
    const float age = static_cast<float>(now - start) * 1e-9f;
    const auto l = KosRitualAt(age);
    if (l.rise <= 0.0f && l.burst <= 0.0f) {
        return {};
    }
    const float seed = static_cast<float>((start / 1000003) % 997) / 997.0f;
    return {l.rise, l.burst, age, seed};
}

inline const ShotWeapon* ShotWeaponOf(std::int32_t variation) {
    static constexpr ShotWeapon table[] = {
        {1010, Shot::GunBlade, 0.867f, 0.0f, 6102, 720, 721, 12100, 12130, true}, // Reiterpallasch
        {1000, Shot::GunBlade, RifleCooldownS, RifleChargeS, 6302, 700, 712, 12100,
         12130, true},                                                     // Rifle Spear
        {2600, Shot::MoonWave, 0.0f, 0.0f, -1, -1, -1, 12100, 12120, true}, // Holy Moonlight Sword
        {1100, Shot::StakeBlast, BlastCooldownS, BlastChargeS, -1, -1, -1, 11110,
         11206},                                                          // Stake Driver
        {3200, Shot::Bow, 0.0f, 0.0f, -1, -1, -1, -1,
         -1}, // Simon's Bowblade (core/vr/bow.h bow_direct)
    };
    for (const ShotWeapon& w : table) {
        if (w.variation == variation) {
            return &w;
        }
    }
    return nullptr;
}

struct ShotLine {
    Body::Vec3 origin{};
    Body::Vec3 direction{};
};

inline ShotLine ShotLineOf(const ShotWeapon& weapon, Body::Vec3 base, Body::Vec3 tip) {
    // Retail 1.09 FLVERs: dummy 100 is farther along the blade than muzzle 130
    // (Rifle Spear/Reiterpallasch) or grip 120 (Moonlight Sword). Their old
    // tip-base ray pointed back into the handle. Keep their existing emission
    // positions: the muzzle for gun-blades, midpoint for the Moonlight wave.
    return {weapon.shot == Shot::MoonWave ? (base + tip) * 0.5f : tip,
            Body::Normalize(weapon.reverse_line ? base - tip : tip - base)};
}

// A verified charged melee row routes the stake discharge to a powered melee window,
// instead of a projectile request that the game cannot fulfill.
constexpr bool StakePunch(std::int32_t variation, bool form_b, bool charged_melee) {
    return variation == 1100 && form_b && charged_melee;
}

// The ordinary swing gate has already verified speed, travel and tracking continuity.
// Only a forward punch (not a backswing or sideways slash) discharges a held stake.
inline bool StakePunchMotion(bool committed, Vec3 relative_velocity, Vec3 forward) {
    const float speed = Body::Length(relative_velocity);
    return committed && Body::IsFinite(relative_velocity) && Body::IsFinite(forward) &&
           speed > 0.5f && Body::LengthSq(forward) > .5f &&
           Body::Dot(relative_velocity, Body::Normalize(forward)) > .5f * speed;
}

// weapon_hum: the sword hand's hum while a transformed weapon runs (by its variation): gentle
// while held (just over the least a Touch motor renders: a steady level gets no haptic floor),
// revving up with the swing.
struct Hum {
    float idle{0.0f}; // held still
    float rev{0.0f};  // added at a full-speed swing
};

constexpr Hum HumOf(std::int32_t variation) {
    return variation == 3100   ? Hum{0.12f, 0.22f} // the Whirligig Saw's spinning blade
           : variation == 2800 ? Hum{0.10f, 0.14f} // the Boom Hammer's lit furnace
                               : Hum{};
}

// How much of the rev a swing gives: the blade's far end from 1 m/s (none) to 5 m/s (all).
constexpr float HumRev(float tip_speed) {
    return std::clamp((tip_speed - 1.0f) / 4.0f, 0.0f, 1.0f);
}

// ---- The Whirligig Saw's spin (the tester's, 2026-10-12) ---------------------------------------------

// Its transformed form (family 310) held with both hands (the two-handed grip on its handle):
// the spin trigger (left normally, right in optimized mode) revs it up over SpinUpS, keeps it
// spinning (a deep rumble with the teeth's grain, in both hands), and let go (or a hand off the
// handle) it spins down over SpinDownS (a decaying whir). Never the game's L2 (its canned step).
// The weapon-only clip and the repeating attack run on the game thread (SawAttackTick).
constexpr std::int32_t SawFamily = 310;
constexpr float SpinUpS = 0.3f;
constexpr float SpinDownS = 0.5f;
constexpr float SpinTeethHz = 14.0f; // the grain at full speed (slower as it winds down)
constexpr float SpinRumble = 0.35f;  // the rumble at full speed ...
constexpr float SpinGrain = 0.12f;   // ... and the teeth's swing above and below it (felt on Touch)

// Attachment keeps the left trigger in the optimized scheme; standard mode frees right for charge.
inline constexpr bool SawSpinTrigger(bool optimized, bool left, bool right) {
    return optimized ? right : left;
}

class SawSpin {
public:
    enum class Event : std::uint8_t {
        None,
        Start, // the trigger squeezed with both hands on it: the pull-start
        Full,  // up to speed
        Stop,  // let go (or a hand off the handle): spinning down
        Still, // stopped
    };
    struct Out {
        Event event{Event::None};
        float rev{0.0f};   // 0 (still) .. 1 (full speed)
        float level{0.0f}; // the feel's level in the hands (0: none)
    };

    Out Step(bool trigger, bool both_hands, float dt) {
        Out out{};
        dt = std::max(dt, 0.0f);
        const bool want = trigger && both_hands;
        if (want != wanted) {
            wanted = want;
            out.event = want ? Event::Start : Event::Stop;
        }
        const float was = rev;
        rev = want ? std::min(rev + dt / SpinUpS, 1.0f) : std::max(rev - dt / SpinDownS, 0.0f);
        if (out.event == Event::None && want && was < 1.0f && rev >= 1.0f) {
            out.event = Event::Full;
        } else if (out.event == Event::None && !want && was > 0.0f && rev <= 0.0f) {
            out.event = Event::Still;
        }
        phase = std::fmod(phase + dt * SpinTeethHz * rev, 1.0f);
        const float grain = phase < 0.5f ? SpinGrain : -SpinGrain;
        out.rev = rev;
        out.level = rev > 0.0f ? rev * (SpinRumble + grain) : 0.0f;
        return out;
    }

    bool Spinning() const {
        return rev > 0.0f;
    }
    void Reset() {
        *this = SawSpin{};
    }

private:
    bool wanted{false};
    float rev{0.0f};
    float phase{0.0f};
};

// Verified against CUSA03173 v1.09: WP_A_1707_2.anibnd contains a148_052910_2.hkx.
// The HKX has 31 samples spanning 1 s; loop that full weapon clip independently
// of the attack cadence. a148.tae 52910 judges 931/932 are 13 frames apart
// (0.1 and 0.5333 s). Only the weapon gets this clip.
constexpr std::int32_t SawLoopClip = 148052910;
constexpr float SawLoopS = 1.0f;
constexpr float SawHitS = 13.0f / 30.0f;
constexpr std::int64_t SawFreshNs = 250'000'000;

constexpr bool SawFresh(std::int64_t ns, std::int64_t now) {
    return ns > 0 && now >= ns && now - ns < SawFreshNs;
}

class SawAttackClock {
public:
    // No catch-up attacks after a hitch. A release immediately ends contact damage;
    // the weapon alone coasts down with rev. Restarting begins a fresh hit interval.
    bool Step(float rev, bool held, float dt) {
        dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
        rev = std::isfinite(rev) ? std::clamp(rev, 0.0f, 1.0f) : 0.0f;
        time = std::fmod(time + dt * rev, SawLoopS);
        if (!held || rev < 1.0f) {
            active = false;
            remaining = 0.0f;
            return false;
        }
        remaining -= dt;
        if (!active || remaining <= 0.0f) {
            active = true;
            remaining = SawHitS;
            return true;
        }
        return false;
    }
    float Time() const { return time; }
    bool Active() const { return active; }

private:
    float time{0.0f};
    float remaining{0.0f};
    bool active{false};
};

// The transformed form's R2 and charged R2 judges (core/vr/melee_moves.h: heavy 550, 910 or 500;
// charged 570 or 930). A shot is the first of them whose behaviour row is a bullet.
inline constexpr std::int32_t HeavyJudgesB[] = {550, 910, 500};
inline constexpr std::int32_t ChargedJudgesB[] = {570, 930};

// ---- The left hand's swings (melee_left) ---------------------------------------------------------

// The right weapons whose transformed form puts a part of them in the left hand (its attacks are
// the left hand's): the Blade of Mercy's (400) and Rakuyo's (2700) second blade, the Beast Claw's
// (900) left claw, the Kos Parasite's (101) parasite, and with the Milkweed rune the tendril
// weapon the game swaps in for it (3800; core/vr/kos_lash.h): its left hand's tendrils.
constexpr bool LeftPartWeapon(std::int32_t variation) {
    return variation == 400 || variation == 2700 || variation == 900 || variation == 101 ||
           variation == 3800;
}

// The left-hand weapons that attack (their L2): the Torch and the Hunter's Torch (2000), the Fist
// of Gratia (3400), the bare fist (100).
constexpr std::int32_t FistVariation = 100;

// Bare fists have one set of physical punch rows. L1 can leave them in style 3,
// including in an existing save; that stance must not select empty form-B rows.
constexpr bool PhysicalMeleeFormB(std::int32_t variation, std::int32_t style) {
    return variation != FistVariation && (style == 3 || style == 4 || style == 5);
}

constexpr bool LeftMeleeWeapon(std::int32_t variation) {
    return variation == 2000 || variation == 3400 || variation == FistVariation;
}

// Guns whose hand punches: Hunter Pistol and Evelyn (1400), Repeating Pistol (1410), Hunter
// Blunderbuss (600), and Piercing Rifle (3600). The punch is the bare fist's (its learned row),
// thrown with the gun in the hand; it does not extend melee reach along the barrel. A
// shield (IsShieldFamily: no attack rows of its own) bashes the same way, including from auto-guard.
constexpr bool PunchGun(std::int32_t variation) {
    return variation == 1400 || variation == 1410 || variation == 600 || variation == 3600;
}

// The left weapon's L2 lookup identifies its equipped variation. Judge 0 can return a
// generic action instead: the live Hunter Pistol returned 101000000 there, but 101400800
// for judge 800. Decode only the requested L2 row; eligibility stays with the allowlists.
constexpr std::int32_t LeftWeaponJudge = 800;
constexpr std::int32_t LeftWeaponVariation(std::int32_t behavior) {
    if (behavior < 100000000 || behavior >= 200000000 ||
        (behavior - 100000000) % 1000 != LeftWeaponJudge) {
        return -1;
    }
    return (behavior - 100000000) / 1000;
}
// Physical left-hand gun punches alone pay half the configured 0..100 stamina percentage.
// `punch` is a gun variation, a negative shield family, or zero for native/dual-wield melee.
// Round odd percentages up so a paying attack never becomes free. Shooting never calls this.
constexpr int GunPunchStaminaPct(std::int32_t punch, int light_pct) {
    return PunchGun(punch) ? (light_pct + 1) / 2 : light_pct;
}

// The left-hand weapons' own L2, known without learning it (melee_left_rows' key, as LeftRowOf):
// BehaviorParam_PC's category-2 (left hand) attack, judge 800, of the bare fist (1000: the gun's
// and the shield's punch too), the Torch (20000) and the Fist of Gratia (34000). Before, a left
// swing attacked only once the game's own L2 had been seen once, which no run ever did: the
// Torch, the punches never hit (2026-10-13). -1: none known (learned in the game, as before).
constexpr std::int32_t KnownLeftRow(std::int32_t key) {
    switch (key) {
    case FistVariation * 10:
    case 2000 * 10:
    case 3400 * 10:
        return 800;
    case 3800 * 10 + 1:
        // The Kos Parasite's tendril weapon (the Milkweed rune), form B's left hand: the game's
        // own first form-B R1 (a156_040000, judge 400) lashes with the left tendrils alone.
        return 400;
    default:
        return -1;
    }
}

// Both hands swing at one pace: the left hand's swings pay from the right hand's combo budget.
// With a left-hand weapon of its own (LeftMeleeWeapon, PunchGun) a left swing within CounterSwingS
// of a right one (either first) is the off hand's counter-motion: dropped, silently and free. The
// right weapon's left part (LeftPartWeapon: the dual blades) swings with it. A gun's punch needs a
// strike this fast (the hand, m/s: the swing detector's stab speed), never with the trigger
// pulled past GunPunchTrigger, nor within GunPunchAfterShotS of a shot (an aim is no punch).
constexpr float CounterSwingS = 0.18f;
// 2.8: a little under the swing detector's stab speed (3.0 until 2026-10-18: the tester, a tiny bit
// less); the left hand's detector takes a gun's punch at this speed too.
constexpr float GunPunchSpeed = 2.8f;
constexpr float GunPunchTrigger = 0.2f;
constexpr float GunPunchAfterShotS = 0.5f;
// Explicit trigger holds still block a bash. Automatic shield raising must not swallow
// a committed strike; the existing speed, classifier and shared combo gates still apply.
constexpr bool PunchAimBlocks(bool shield, float trigger, bool recent_shot) {
    return trigger > GunPunchTrigger || (!shield && recent_shot);
}

// The left hand's learned attack rows (vr.ini melee_left_rows, written by the game as it learns
// one: core/vr/bloodborne_melee.cpp ObserveLeftRow), so a weapon keeps its row through its
// transforms and from session to session: "key:judge,..." with key = variation * 10 + 1 for the
// right weapon's left part, + 0 for a left-hand weapon. -1: none for that key.
inline std::int32_t LeftRowOf(std::string_view rows, std::int32_t key) {
    while (!rows.empty()) {
        const std::size_t comma = rows.find(',');
        const std::string_view item = rows.substr(0, comma);
        rows = comma == std::string_view::npos ? std::string_view{} : rows.substr(comma + 1);
        const std::size_t colon = item.find(':');
        std::int32_t k = 0;
        std::int32_t judge = 0;
        if (colon == std::string_view::npos ||
            std::from_chars(item.data(), item.data() + colon, k).ptr != item.data() + colon ||
            std::from_chars(item.data() + colon + 1, item.data() + item.size(), judge).ptr !=
                item.data() + item.size()) {
            continue; // malformed
        }
        if (k == key) {
            return judge;
        }
    }
    return -1;
}

// `rows` with `key`'s row set to `judge` (the others kept, in order; malformed items dropped).
inline std::string WithLeftRow(std::string_view rows, std::int32_t key, std::int32_t judge) {
    std::string out;
    while (!rows.empty()) {
        const std::size_t comma = rows.find(',');
        const std::string_view item = rows.substr(0, comma);
        rows = comma == std::string_view::npos ? std::string_view{} : rows.substr(comma + 1);
        const std::size_t colon = item.find(':');
        std::int32_t k = 0;
        if (colon == std::string_view::npos ||
            std::from_chars(item.data(), item.data() + colon, k).ptr != item.data() + colon ||
            k == key || LeftRowOf(item, k) < 0) {
            continue;
        }
        out.append(item).push_back(',');
    }
    return out + std::to_string(key) + ":" + std::to_string(judge);
}

// Where a left swing's attack hangs, from its capsule's end against the two fists (m): from the
// left one (closer to it by 10 cm, within 1.3 m) or the right one (closer to that by 10 cm: the
// right hand's row); else not known yet (the hands together, or far from both).
enum class CapsuleSide : std::uint8_t { Unsure, Left, Right };

constexpr CapsuleSide SideOf(float to_left, float to_right) {
    if (to_left < 1.3f && to_left + 0.10f < to_right) {
        return CapsuleSide::Left;
    }
    if (to_right + 0.10f < to_left) {
        return CapsuleSide::Right;
    }
    return CapsuleSide::Unsure;
}

// The right weapon's left part (LeftPartWeapon) learns its row from your own first left swings
// (physical swings never let the game show one): each candidate row's attack is opened in turn and
// kept when its hitbox shows at the left fist (Learned), ended when at the right one (Wrong: the
// next candidate), and given up for this swing after `frames_max` updates with no side seen (no
// hitbox, the fists together: Unseen). Pure (core/vr/bloodborne_melee.cpp CheckLeftProbe).
// ProbeFrames is tuned at 60 fps: the caller passes it for the game's frame rate (the hitbox comes
// on the attack's own clock, as long in time at any rate: VR::GameFramesAt60).
enum class Probe : std::uint8_t { Wait, Learned, Wrong, Unseen };
constexpr std::uint32_t ProbeFrames = 6;

constexpr Probe ProbeStep(bool seen, float to_left, float to_right, std::uint32_t frames,
                          std::uint32_t frames_max = ProbeFrames) {
    if (seen) {
        const CapsuleSide side = SideOf(to_left, to_right);
        if (side != CapsuleSide::Unsure) {
            return side == CapsuleSide::Left ? Probe::Learned : Probe::Wrong;
        }
    }
    return frames >= frames_max ? Probe::Unseen : Probe::Wait;
}

// ---- Geometry -------------------------------------------------------------------------------------

// A point against the head: in front of the eyes (+), above them (+), to the right (+). `forward`
// is the head's horizontal forward (unit).
struct HeadLocal {
    float fwd{0.0f};
    float up{0.0f};
    float side{0.0f};
};

inline HeadLocal InHead(Vec3 head, Vec3 forward, Vec3 p) {
    const Vec3 d = p - head;
    const Vec3 right = Body::Cross(forward, {0.0f, 1.0f, 0.0f});
    return {Body::Dot(d, forward), d.y, Body::Dot(d, right)};
}

// The point of the line from `a` along `axis` (unit), between s0 and s1 m, nearest to `p`.
struct Nearest {
    float s{0.0f}; // how far along the axis
    float d{0.0f}; // how far from p
};

inline Nearest NearestOnAxis(Vec3 a, Vec3 axis, float s0, float s1, Vec3 p) {
    const float s = std::clamp(Body::Dot(p - a, axis), s0, s1);
    return {s, Body::Length(p - (a + axis * s))};
}

// The Holy Moonlight Sword's wave: out from the chest through the blade's middle, never steeper
// than `max_pitch_deg` (straight above or below the chest: the head's forward).
inline Vec3 WaveAim(Vec3 chest, Vec3 blade_mid, Vec3 forward, float max_pitch_deg = 20.0f) {
    const Vec3 d = blade_mid - chest;
    Vec3 flat{d.x, 0.0f, d.z};
    const float run = Body::Length(flat);
    flat = run > 0.05f ? flat * (1.0f / run) : forward;
    const float limit = max_pitch_deg * Body::Deg;
    const float pitch = std::clamp(std::atan2(d.y, std::max(run, 0.05f)), -limit, limit);
    return flat * std::cos(pitch) + Vec3{0.0f, std::sin(pitch), 0.0f};
}

// The moonlight wave (2026-11-10: "It should fly out of the sword when it crosses our
// vision vector"; "think of the vision check like rotated along the horizon"; "the horizontal angle
// is more important than the vertical one"; "I have an enemy in front of me. I swing my weapon, and
// a beam flies off and hits the enemy"). A charged stroke lets it go:
// - the moment the blade sweeps across where you face (ViewCrossing): its middle, seen from your
//   eyes, passing your view's heading left to right or right to left, at any height (looking up
//   or down changes nothing); a chop in front of you (within ChopYawDeg of your heading), which
//   never crosses it sideways, as it comes down through your eye line. Out along your view's
//   heading, its up/down the sword's own, not your eyes' (MoonWaveLine: look up, swing a level
//   sword, the wave flies level); a chop's flies level;
// - a stroke that never crosses (beside you, or with your eyes elsewhere): when you let go of the
//   trigger while it is under way (its far end past MoonMinPeakMps), along the blade, hilt to tip
//   (BladeWaveAim).
// Its up/down is pulled HorizonPullDeg toward level (within it: level; 10 at first, 15 since the
// user's "a tiny bit more" toward the horizon) and never steeper than WaveMaxPitchDeg; its
// crescent, tilted as your slash, drawn flat within FlatCrescentDeg of flat (LevelSlash). Either way the game side turns it straight at the target the game picked (locked
// on or not) when that target is in front of the hunter, within reach and not clearly away from
// where it was sent (MoonWaveAim): a swing at an enemy hits it.
constexpr float ChopYawDeg = 15.0f;
constexpr float HorizonPullDeg = 15.0f;
constexpr float FlatCrescentDeg = 10.0f;
constexpr float WaveMaxPitchDeg = 20.0f;
constexpr float MoonMinPeakMps = 1.5f; // a stroke under way, not a twitch
// The blade's middle nearer your eyes than this, seen from above (right over your head at the top of
// an overhead chop): its heading is noise there, a hand's wobble flips it; and a heading that jumps
// more than CrossingJumpDeg in a frame went over or past you, not across: neither is a crossing
// (review, 2026-11-10: a centred chop fired at its top, up into the air).
constexpr float CrossingMinM = 0.15f;
constexpr float CrossingJumpDeg = 60.0f;
// A slash crosses from a side: the blade had been at least this far out on the side it leaves
// (a hand's wobble about your heading, as a chop comes over, never is).
constexpr float SlashArmDeg = 8.0f;
// Where the blade crosses: checked between frames (a fast slash moves the blade ~14 degrees a
// frame: a point test would skip over it). Step every frame of the stroke; Slash on the frame its
// middle passed your view's heading (in front of you), Chop on the frame a blade in front of you
// passed your eye line; None otherwise.
struct ViewCrossing {
    enum class Kind { None, Slash, Chop };
    bool have{false};
    int side{0};       // the side it has been on, SlashArmDeg out: -1 left, +1 right, 0 neither
    float yaw{0.0f};   // the blade's heading off your view's (radians, + to your right)
    float pitch{0.0f}; // its height off your eye line, seen from your eyes (radians)
    Kind Step(Vec3 eye, Vec3 gaze, Vec3 point) {
        const Vec3 to = point - eye;
        const Vec3 flat_gaze{gaze.x, 0.0f, gaze.z};
        const Vec3 flat_to{to.x, 0.0f, to.z};
        if (!Body::IsFinite(to) || !Body::IsFinite(gaze) || Body::LengthSq(flat_gaze) < 1e-4f ||
            Body::LengthSq(flat_to) < CrossingMinM * CrossingMinM) {
            have = false; // right over your head, or at your eyes: start afresh
            side = 0;
            return Kind::None;
        }
        const Vec3 g = Body::Normalize(flat_gaze);
        const float ahead = Body::Dot(flat_to, g);
        const float right = flat_to.x * -g.z + flat_to.z * g.x; // (y up: the heading's right)
        const float now_yaw = std::atan2(right, ahead);
        const float now_pitch = std::atan2(to.y, Body::Length(flat_to));
        const bool had = have;
        const float from_yaw = yaw, from_pitch = pitch;
        const int from_side = side;
        have = true;
        yaw = now_yaw;
        pitch = now_pitch;
        const float arm = SlashArmDeg * Body::Deg;
        if (std::abs(now_yaw) < 0.5f * Body::Pi && std::abs(now_yaw) >= arm) {
            side = now_yaw < 0.0f ? -1 : 1;
        }
        if (!had) {
            return Kind::None;
        }
        // In front both frames (the sign of a heading flips behind you too: never that), and no
        // jump (CrossingJumpDeg).
        const float quarter = 0.5f * Body::Pi;
        if (std::abs(from_yaw) >= quarter || std::abs(now_yaw) >= quarter ||
            std::abs(now_yaw - from_yaw) > CrossingJumpDeg * Body::Deg) {
            return Kind::None;
        }
        if ((from_yaw < 0.0f) != (now_yaw < 0.0f) && from_side == (from_yaw < 0.0f ? -1 : 1)) {
            return Kind::Slash;
        }
        const float chop = ChopYawDeg * Body::Deg;
        if (std::abs(from_yaw) <= chop && std::abs(now_yaw) <= chop &&
            (from_pitch < 0.0f) != (now_pitch < 0.0f)) {
            return Kind::Chop;
        }
        return Kind::None;
    }
};
// Let go mid-stroke: fires once the stroke is under way.
constexpr bool MoonReleaseFires(float tip_peak, bool trigger_released) {
    return trigger_released && tip_peak >= MoonMinPeakMps;
}
// An up/down angle pulled HorizonPullDeg toward level (within it: level), at most WaveMaxPitchDeg.
inline float TowardHorizon(float pitch) {
    const float pull = HorizonPullDeg * Body::Deg;
    const float most = WaveMaxPitchDeg * Body::Deg;
    const float magnitude = std::clamp(std::abs(pitch) - pull, 0.0f, most);
    return pitch < 0.0f ? -magnitude : magnitude;
}
// The way `heading` points (flat), at `pitch` (radians).
inline Vec3 HeadingAt(Vec3 heading, float pitch) {
    Vec3 flat{heading.x, 0.0f, heading.z};
    flat = Body::Normalize(flat, Vec3{0.0f, 0.0f, -1.0f});
    return flat * std::cos(pitch) + Vec3{0.0f, std::sin(pitch), 0.0f};
}
// The crossing's flight: your view's heading (the forward when you look straight up or down), the
// sword's up/down toward the horizon (a chop: level).
inline Vec3 MoonWaveLine(Vec3 gaze, Vec3 blade_axis, Vec3 forward, bool chop) {
    const Vec3 flat{gaze.x, 0.0f, gaze.z};
    const Vec3 heading = Body::LengthSq(flat) > 0.01f ? flat : forward;
    const Vec3 axis = Body::Normalize(blade_axis, forward);
    const float pitch = chop ? 0.0f : TowardHorizon(std::asin(std::clamp(axis.y, -1.0f, 1.0f)));
    return HeadingAt(heading, pitch);
}
// Along the blade, hilt to tip, its up/down toward the horizon (an overhead chop ends pointing
// down: the wave goes along the ground, not into it). A blade straight up or down: the forward.
inline Vec3 BladeWaveAim(Vec3 blade_axis, Vec3 forward) {
    const Vec3 axis = Body::Normalize(blade_axis, forward);
    const Vec3 flat{axis.x, 0.0f, axis.z};
    const Vec3 heading = Body::Length(flat) > 0.05f ? flat : forward;
    return HeadingAt(heading, TowardHorizon(std::asin(std::clamp(axis.y, -1.0f, 1.0f))));
}
// The crescent's tilt (the slash across the flight): within FlatCrescentDeg of flat, flat.
inline Vec3 LevelSlash(Vec3 slash) {
    const float length = Body::Length(slash);
    if (!(length > 1e-4f) || !Body::IsFinite(slash)) {
        return slash;
    }
    const Vec3 n = slash * (1.0f / length);
    if (std::abs(std::asin(std::clamp(n.y, -1.0f, 1.0f))) > FlatCrescentDeg * Body::Deg) {
        return slash;
    }
    const Vec3 flat{n.x, 0.0f, n.z};
    return Body::LengthSq(flat) > 1e-6f ? Body::Normalize(flat) * length : slash;
}
// The target in front: within MoonTargetFrontDeg of the hunter's facing (flat), MoonTargetRangeM
// of the wave's start (it reaches ~6.6 m: two bullets at 20 m/s, row 590/591 and their flight) and
// MoonTargetAwayDeg of where the wave was sent (one sent clearly elsewhere goes there).
constexpr float MoonTargetRangeM = 8.0f;
constexpr float MoonTargetFrontDeg = 60.0f;
constexpr float MoonTargetAwayDeg = 90.0f;
struct MoonAim {
    Vec3 dir{};
    bool at_target{false};
    float off_deg{0.0f}; // where it was sent, from the target (degrees; with a target in reach)
};
inline MoonAim MoonWaveAim(Vec3 sent, Vec3 origin, Vec3 target, bool have_target, Vec3 facing) {
    MoonAim out{};
    out.dir = Body::Normalize(sent, facing);
    if (!have_target || !Body::IsFinite(out.dir) || !Body::IsFinite(origin) ||
        !Body::IsFinite(target) || !Body::IsFinite(facing)) {
        return out;
    }
    const Vec3 to = target - origin;
    const float distance = Body::Length(to);
    if (!(distance >= 0.5f && distance <= MoonTargetRangeM)) {
        return out;
    }
    const Vec3 flat_to{to.x, 0.0f, to.z};
    const Vec3 flat_facing{facing.x, 0.0f, facing.z};
    if (Body::LengthSq(flat_to) < 1e-6f || Body::LengthSq(flat_facing) < 1e-6f ||
        Body::Angle(Body::Normalize(flat_to), Body::Normalize(flat_facing)) >
            MoonTargetFrontDeg * Body::Deg) {
        return out;
    }
    const Vec3 to_dir = to * (1.0f / distance);
    out.off_deg = Body::Angle(out.dir, to_dir) / Body::Deg;
    if (out.off_deg > MoonTargetAwayDeg) {
        return out;
    }
    out.dir = to_dir;
    out.at_target = true;
    return out;
}

// Where you look (tool_aim; the moonlight wave has its own: ViewCrossing): a motion that passed
// through your line of sight (within GazeDeg of where the head points) lets its shot go there; the Augur's within GazeS before its
// burst leaves (the item clip's delay).
constexpr float GazeDeg = 20.0f;
constexpr float GazeS = 0.8f;

// tool_aim for the hunter tools that fire something (the tester's 2026-10-30 "yes we need this too.
// And done well and carefully"), from the 1.09 params (EquipParamGoods refId -> BulletParam):
//  - Full (the line where you aim): the Augur of Ebrietas' burst (bullet 370: 7 m/s, 6 m), the
//    Blacksky Eye's meteor (460: 30 m/s, 16 m, its impact 461), the Accursed Brew's skull (470:
//    15-20 m/s, 7.6 m, a little gravity, its blast 471);
//  - Heading (only its heading where you aim, the game's own pitch kept): Tiny Tonitrus' bolt (410:
//    33 m/s and a gravity of 500, so it falls at once, then 18 links of lightning along the
//    ground, 411-428, each from the last: only the first is turned, the chain follows it).
// Only a tool's first bullet is aimed, never its children (an impact keeps where it hit). Not
// aimed: the bullets that seek on their own (A Call Beyond's 24 stars, 380/381; the Executioner's
// Gloves' 3 spirits, 350), those round the hunter (Beast Roar 390, the Choir Bell 360) and the
// Madaras Whistle's snake (480-484: it rises at its dummy by the hunter).
enum class ToolAimKind : std::uint8_t { None, Full, Heading };
struct ToolAimInfo {
    std::int32_t goods{-1};
    std::int32_t bullet{-1}; // its first bullet (the launch's BulletParam id)
    ToolAimKind kind{ToolAimKind::None};
    const char* name{""};
};
inline constexpr std::array<ToolAimInfo, 4> ToolAims{{
    {2000, 370, ToolAimKind::Full, "the Augur's burst"},
    {2120, 460, ToolAimKind::Full, "the Blacksky Eye's meteor"},
    {2130, 470, ToolAimKind::Full, "the Accursed Brew's skull"},
    {2070, 410, ToolAimKind::Heading, "Tiny Tonitrus' lightning"},
}};
constexpr ToolAimInfo ToolAimOfBullet(std::int32_t bullet) {
    for (const ToolAimInfo& t : ToolAims) {
        if (t.bullet == bullet) {
            return t;
        }
    }
    return {};
}
constexpr ToolAimInfo ToolAimOfGoods(std::int32_t goods) {
    for (const ToolAimInfo& t : ToolAims) {
        if (t.goods == goods) {
            return t;
        }
    }
    return {};
}
// Where it is aimed: where you look when the left hand pointed there within GazeS (thrust at what
// you look at; it need not still point there as it leaves), else where the hand points; a
// Heading tool where you look (Tiny Tonitrus is struck down: the hand points at the floor then),
// else the hand's heading. False: neither known (the game's own aim).
enum class ToolAimFrom : std::uint8_t { None, Gaze, Hand };
inline ToolAimFrom ToolAimSource(ToolAimKind kind, bool gaze_ok, bool pointed_at_gaze, Vec3 gaze,
                                 bool hand_ok, Vec3 hand, Vec3& aim) {
    const bool flat_hand = hand_ok && std::hypot(hand.x, hand.z) > 0.2f;
    if (gaze_ok && (kind == ToolAimKind::Heading || pointed_at_gaze)) {
        aim = gaze;
        return ToolAimFrom::Gaze;
    }
    if (hand_ok && (kind == ToolAimKind::Full || flat_hand)) {
        aim = hand;
        return ToolAimFrom::Hand;
    }
    if (gaze_ok) {
        aim = gaze;
        return ToolAimFrom::Gaze;
    }
    return ToolAimFrom::None;
}
// The bullet's new line from the game's (`vanilla`) and the aim: Full the aim itself; Heading the
// aim's heading at the game's own pitch (a near-vertical aim has no heading: the game's).
inline Vec3 ToolAimLine(ToolAimKind kind, Vec3 vanilla, Vec3 aim) {
    const Vec3 v = Body::Normalize(vanilla, Vec3{0.0f, 0.0f, 1.0f});
    if (kind == ToolAimKind::Full) {
        return Body::Normalize(aim, v);
    }
    const float run = std::hypot(aim.x, aim.z);
    if (kind != ToolAimKind::Heading || run < 0.2f) {
        return v;
    }
    const float pitch_y = std::clamp(v.y, -1.0f, 1.0f);
    const float level = std::sqrt(std::max(0.0f, 1.0f - pitch_y * pitch_y));
    return Vec3{aim.x / run * level, pitch_y, aim.z / run * level};
}
// `line` turned down by `deg` (up for a negative one), its heading kept; a near-vertical line has
// no heading to turn about and stays as is. Never turned past 86 degrees either way.
inline Vec3 PitchedDown(Vec3 line, float deg) {
    const Vec3 d = Body::Normalize(line, Vec3{0.0f, 0.0f, 1.0f});
    const float run = std::hypot(d.x, d.z);
    if (run < 0.05f || deg == 0.0f || !std::isfinite(deg)) {
        return d;
    }
    const float was = std::atan2(d.y, run);
    const float pitch = std::clamp(was - deg * Body::Deg, std::min(was, -1.5f), std::max(was, 1.5f));
    const float level = std::cos(pitch);
    return Vec3{d.x / run * level, std::sin(pitch), d.z / run * level};
}
// Locked on, an aim within ToolLockKeepDeg of the game's own line (the game aims a tool at its lock
// target within 30 degrees of the hunter's facing) keeps the game's: a tool thrust at the target
// you are locked on to hits it; one aimed elsewhere goes there.
constexpr float ToolLockKeepDeg = 10.0f;
inline bool ToolKeepsLock(bool locked, Vec3 vanilla, Vec3 line) {
    return locked && Body::Angle(Body::Normalize(vanilla, line), Body::Normalize(line, vanilla)) <=
                         ToolLockKeepDeg * Body::Deg;
}

// Orientation only: sample blade-midpoint motion relative to the head, with a short filter.
// Freeze the slash at peak speed, before the follow-through can change its plane.
struct WaveSlash {
    Vec3 previous{}, filtered{}, at_peak{}, normal_at_peak{};
    float peak{0.0f};
    bool have_previous{false};

    void Step(Vec3 relative_mid, float dt, float speed, Vec3 blade_axis = {}) {
        if (!Body::IsFinite(relative_mid) || !std::isfinite(dt) || dt <= 0.0f || dt > 0.1f) {
            *this = {};
            return;
        }
        if (have_previous) {
            const Vec3 v = (relative_mid - previous) * (1.0f / dt);
            if (Body::IsFinite(v) && Body::Length(v) <= 40.0f) {
                filtered = filtered + (v - filtered) * (1.0f - std::exp(-dt / 0.025f));
                if (std::isfinite(speed) && speed >= 1.5f && speed > peak &&
                    Body::Length(filtered) >= 0.5f) {
                    peak = speed;
                    at_peak = filtered;
                    // A chop can travel straight toward the target at its fastest. Its blade
                    // axis still identifies the vertical sweep plane then.
                    normal_at_peak = {};
                    if (Body::IsFinite(blade_axis)) {
                        const Vec3 normal = Body::Cross(Body::Normalize(blade_axis),
                                                       Body::Normalize(filtered));
                        if (Body::LengthSq(normal) >= 0.04f)
                            normal_at_peak = Body::Normalize(normal);
                    }
                }
            } else {
                filtered = {};
                at_peak = {};
                normal_at_peak = {};
                peak = 0.0f;
            }
        }
        previous = relative_mid;
        have_previous = true;
    }

    Vec3 Across(Vec3 flight) const {
        const Vec3 across = Body::Cross(normal_at_peak, Body::Normalize(flight));
        return Body::IsFinite(across) && Body::LengthSq(across) >= 0.04f
                   ? across : at_peak;
    }
};

// ---- Hand transforms --------------------------------------------------------------------------------

struct Hand {
    bool valid{false}; // its position known: tracked, or inferred where the cameras lose it (over
                       // the shoulder, at the cheek), which the grabs and the shield go by
    Vec3 p{};          // the grip (m)
    Vec3 v{};          // m/s (only when tracked)
    bool grip{false};  // squeezed (the caller's hysteresis)
    bool tracked{false}; // seen by the cameras, with a velocity: the moving gestures go by it
};

struct Frame {
    float dt{0.0f};
    Hand left{};
    Hand right{};
    Vec3 head{};
    Vec3 forward{0.0f, 0.0f, -1.0f}; // the head's horizontal forward (unit)
    Vec3 axis{};      // the right weapon: from the fist toward its far end (unit; zero: unknown)
    Vec3 left_axis{}; // the left blade of the dual blades' form B (zero: unknown)
    float left_reach{0.0f}; // what the left hand holds reaches this far along left_axis (m; 0: the
                            // bare hand; LeftReachOf)
};

enum class Gesture : std::uint8_t {
    None,
    Grab,  // the left hand took the dual blades' second hilt
    Let,   // ... and let go, or waited too long, without the split
    Split, // the dual blades pulled apart (A -> B)
    Join,  // ... locked together (B -> A)
    Stab,  // the Bloodletter driven into the chest (L1)
};

inline const char* GestureName(Gesture g) {
    switch (g) {
    case Gesture::None:
        return "none";
    case Gesture::Grab:
        return "grab";
    case Gesture::Let:
        return "let go";
    case Gesture::Split:
        return "split";
    case Gesture::Join:
        return "join";
    case Gesture::Stab:
        return "stab";
    }
    return "?";
}

// Whether a gesture is the weapon's transform (the others are the hilt's grab and its end).
constexpr bool Transforms(Gesture g) {
    return g == Gesture::Split || g == Gesture::Join || g == Gesture::Stab;
}

// Where and how fast (m, m/s, s).
constexpr float ReachM = 0.18f;       // the split's grab: the left grip this close to the right
constexpr float JoinM = 0.22f;        // the join: the hands this close (two Touch controllers'
                                      //     rings meet at about 13 cm: room before they clack) ...
constexpr float JoinDeg = 35.0f;      // ... and the blades this parallel
constexpr float NearM = 0.25f;        // a squeeze with the hands this close is the split's or the
                                      //     join's (never the quick items') ...
constexpr float SqueezeS = 0.3f;      // ... and takes hold if they meet within this long
constexpr float SplitTimeoutS = 1.5f; // a hilt held this long without the pull: let go
constexpr float ChestDownM = 0.32f;   // the chest: this far below the eyes
constexpr float StabFromM = 0.30f;    // the Bloodletter's head: this far along the weapon ...
constexpr float StabToM = 0.95f;      // ... to this far
constexpr float StabM = 0.16f;        // ... reaching this close to the chest (so pointing at
                                      //     it: within 32 degrees of the weapon's line) ...
constexpr float StabSpeed = 0.6f;     // ... this fast, the fist driving it along its line (a
                                      //     wind-up sweeping past the chest is none)
constexpr float StabTravelM = 0.10f; // a deliberate inward thrust, even starting near the chest
constexpr float StabBackM = 0.25f;   // or its back end (the hilt): up to this far behind the fist,
                                     //     driven back-first into the chest or the face
constexpr float LostS = 0.3f;         // a hand not tracked this long ends a grab
constexpr float CooldownS = 1.5f;     // after a gesture, no other

// The Bloodletter stab uses only the right arm; splitting/joining dual blades needs both.
constexpr bool HandTransformArmsReady(Kind kind, float left_weight, float right_weight) {
    return right_weight >= 0.5f &&
           (kind == Kind::Bloodletter || (kind == Kind::DualBlades && left_weight >= 0.5f));
}

class HandTransform {
public:
    struct Out {
        Gesture gesture{Gesture::None};
        bool owns_grip{false}; // the left grip's squeeze is the gesture's (never the quick items')
        bool holding{false};   // the hilt is held ...
        float progress{0.0f};  // ... its way to the split (0 .. 1)
        bool missed{false};    // a squeeze for the split or the join that never took (a soft no)
    };

    // Back to idle, the left grip as it is now (a grip already held is no squeeze).
    void Arm(bool grip_now) {
        *this = HandTransform{};
        prev_grip = grip_now;
        owns = false;
    }

    // `ready`: the right grip freed the left hand (the split) or armed the stab (RightGrip); the
    // join needs nothing.
    Out Step(const Frame& f, Kind kind, bool form_b, bool ready, const Settings& s) {
        Out out{};
        const bool grip = f.left.grip;
        const bool edge = grip && !prev_grip;
        prev_grip = grip;
        cooldown = std::max(cooldown - f.dt, 0.0f);
        if (!grip) {
            owns = false;
        }
        if (kind != last_kind || form_b != last_form_b) {
            last_kind = kind;
            last_form_b = form_b;
            held = false;
            hands_near = false;
            prev_valid = false;
        }
        const bool hands = f.left.valid && f.right.valid;
        switch (kind) {
        case Kind::DualBlades:
            if (form_b) {
                Join(f, grip, edge, hands, out);
            } else if (ready || held) {
                Split(f, s, grip, edge, hands, out);
            }
            break;
        case Kind::Bloodletter:
            if (!form_b && ready && f.right.grip) {
                Stab(f, out);
            } else {
                prev_valid = false; // (no speed across the arming)
            }
            break;
        case Kind::None:
            held = false;
            break;
        }
        if (Transforms(out.gesture)) {
            cooldown = CooldownS;
            held = false;
        }
        out.owns_grip = owns;
        return out;
    }

private:
    float Apart(const Frame& f) const {
        return Body::Length(f.left.p - f.right.p);
    }

    // The hilt held: false (and Let) when it ended without the split.
    bool Holding(const Frame& f, bool grip, bool hands, float timeout_s, Out& out) {
        lost = hands ? 0.0f : lost + f.dt;
        held_s += f.dt;
        if (!grip || lost > LostS || held_s > timeout_s) {
            held = false;
            out.gesture = Gesture::Let;
            return false;
        }
        out.holding = true;
        return true;
    }

    // A squeeze for the split or the join: one begun with the hands within NearM is the gesture's
    // (owned: never the quick items'), and takes hold the first frame `at` (the hands where the
    // gesture wants them) within SqueezeS of it; one that never gets there is `missed`.
    bool Squeezed(const Frame& f, bool grip, bool edge, bool hands, bool at, Out& out) {
        if (edge) {
            hands_near = cooldown <= 0.0f && hands && Apart(f) <= NearM;
            near_s = 0.0f;
            owns = owns || hands_near;
        } else if (hands_near) {
            near_s += f.dt;
        }
        if (!hands_near) {
            return false;
        }
        if (hands && at) {
            hands_near = false;
            return true;
        }
        if (!grip || near_s > SqueezeS) {
            hands_near = false;
            out.missed = true;
        }
        return false;
    }

    void Split(const Frame& f, const Settings& s, bool grip, bool edge, bool hands, Out& out) {
        if (!held) {
            if (Squeezed(f, grip, edge, hands, Apart(f) <= ReachM, out)) {
                held = true;
                owns = true;
                held_s = 0.0f;
                lost = 0.0f;
                apart0 = Apart(f);
                out.gesture = Gesture::Grab;
            }
            return;
        }
        if (!Holding(f, grip, hands, SplitTimeoutS, out) || !hands) {
            return;
        }
        const float pulled = Apart(f) - apart0;
        out.progress = std::clamp(pulled / s.pull_m, 0.0f, 1.0f);
        if (pulled >= s.pull_m) {
            out.gesture = Gesture::Split;
        }
    }

    void Join(const Frame& f, bool grip, bool edge, bool hands, Out& out) {
        const bool axes = Body::LengthSq(f.axis) > 0.5f && Body::LengthSq(f.left_axis) > 0.5f;
        const bool side_by_side =
            Apart(f) <= JoinM && (!axes || Body::Angle(f.axis, f.left_axis) <= JoinDeg * Body::Deg);
        if (Squeezed(f, grip, edge, hands, side_by_side, out)) {
            out.gesture = Gesture::Join;
        }
    }

    // The weapon's head (StabFromM .. StabToM along it) closing on the chest: driven into it.
    // Either end into yourself: its head (turned round, driven head-first into the chest) or its
    // back, the hilt end (held as it is, driven back-first into the chest or the face). The same
    // speed and travel along the weapon's line either way: resting it against you is no stab.
    void Stab(const Frame& f, Out& out) {
        const Vec3 chest = f.head + Vec3{0.0f, -ChestDownM, 0.0f};
        if (Body::LengthSq(f.axis) < 0.5f || !f.right.valid || !f.right.tracked || f.dt <= 0.0f) {
            prev_valid = false;
            return;
        }
        const Nearest n = NearestOnAxis(f.right.p, f.axis, StabFromM, StabToM, chest);
        const float back = std::min(NearestOnAxis(f.right.p, f.axis, -StabBackM, 0.0f, chest).d,
                                    NearestOnAxis(f.right.p, f.axis, -StabBackM, 0.0f, f.head).d);
        const Vec3 relative = f.right.p - chest;
        const float advance = prev_valid ? Body::Dot(relative - prev_stab_p, f.axis) : 0.0f;
        // Measure head-relative motion too: leaning onto a stationary weapon is not a stab.
        const float along_v = Body::Dot(f.right.v, f.axis);
        const bool driving = prev_valid && advance / f.dt >= StabSpeed && along_v >= StabSpeed;
        const bool backing = prev_valid && -advance / f.dt >= StabSpeed && -along_v >= StabSpeed;
        stab_travel = driving ? stab_travel + advance : 0.0f;
        back_travel = backing ? back_travel - advance : 0.0f;
        prev_stab_p = relative;
        prev_valid = true;
        if (cooldown <= 0.0f && ((n.d <= StabM && stab_travel >= StabTravelM) ||
                                 (back <= StabM && back_travel >= StabTravelM))) {
            out.gesture = Gesture::Stab;
        }
    }

    bool prev_grip{false};
    bool owns{false};
    float cooldown{0.0f};
    Kind last_kind{Kind::None};
    bool last_form_b{false};
    // The hilt.
    bool held{false};
    float held_s{0.0f};
    float lost{0.0f};
    float apart0{0.0f};
    // A squeeze waiting for the hands to meet (Squeezed).
    bool hands_near{false};
    float near_s{0.0f};
    // The stab.
    bool prev_valid{false};
    Vec3 prev_stab_p{};
    float stab_travel{0.0f};
    float back_travel{0.0f}; // ... back-first
};

// ---- The right grip (GripUseOf) ---------------------------------------------------------------------

constexpr float FreeS = 5.0f;       // freed left hand: waits this long for the grab or hilt
constexpr float LiftM = 0.25f;      // Tonitrus: the fist rises this much from its lowest since the
                                    //     grip closed ...
constexpr float LiftAboveM = 0.05f; // ... to this far above the eyes ...
constexpr float LiftUp = 0.5736f;   // ... the weapon pointing up (its line's up: within 55 degrees,
                                    //     cos 55; the tester, 2026-11-10; 60 before; a raised wrist
                                    //     rarely gets the blade much nearer vertical)

// The Boom Hammer's furnace, struck while the right grip is held (2026-11-06: "grip
// button + strike it with our left hand/left hand weapon"): it lights at once, as the held
// HoldSOf does. Its head runs FurnaceFromM..FurnaceToM out from the fist (the weight 65 cm out);
// a strike is the left hand's line (Frame::left_reach) coming in from outside FurnaceR of it,
// the gap closing at StrikeSpeed at least (a hand resting on it, or drifting in, is none).
constexpr float FurnaceFromM = 0.55f, FurnaceToM = 0.90f, FurnaceR = 0.12f;
constexpr float StrikeSpeed = 1.0f; // m/s (the head's own swing into the hand counts too)
constexpr float BoomStrikeWindowS = 0.28f; // after each cold starter CHUFF, before it catches

constexpr std::int32_t BoomFamily = 280; // the Boom Hammer

constexpr bool StrikeLights(std::int32_t family) {
    return family == BoomFamily;
}

// How far the left hand's gun, torch or shield reaches out from the fist (m; 0: none, the hand).
constexpr float LeftReachOf(std::int32_t family) {
    switch (family) {
    case 60: case 61: case 150: case 330: case 350: case 360: // the long guns
        return 0.45f;
    case 140: case 141: case 142: case 180: case 181: // the pistols, Rosmarinus, the Flamesprayer
        return 0.25f;
    case 200: case 201: // the torches
        return 0.40f;
    case 190: case 191: // the shields
        return 0.20f;
    default:
        return 0.0f;
    }
}

// The gap between the segments a0-a1 and b0-b1 (m; either may be a point).
inline float SegmentGap(Vec3 a0, Vec3 a1, Vec3 b0, Vec3 b1) {
    const Vec3 u = a1 - a0, v = b1 - b0, w = a0 - b0;
    const float a = Body::Dot(u, u), b = Body::Dot(u, v), c = Body::Dot(v, v);
    const float d = Body::Dot(u, w), e = Body::Dot(v, w);
    if (c <= 1e-9f) { // b a point
        const float s = a > 1e-9f ? std::clamp(-d / a, 0.0f, 1.0f) : 0.0f;
        return Body::Length(a0 + u * s - b0);
    }
    const float den = a * c - b * b;
    float s = den > 1e-9f ? std::clamp((b * e - c * d) / den, 0.0f, 1.0f) : 0.0f;
    float t = (b * s + e) / c;
    if (t < 0.0f) {
        t = 0.0f;
        s = a > 1e-9f ? std::clamp(-d / a, 0.0f, 1.0f) : 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
        s = a > 1e-9f ? std::clamp((b - d) / a, 0.0f, 1.0f) : 0.0f;
    }
    return Body::Length(a0 + u * s - (b0 + v * t));
}

class RightGrip {
public:
    enum class Event : std::uint8_t {
        None,
        Freed,   // Free: the left hand is free for the weapon (the latch's click)
        Armed,   // Arm: the stab is armed
        Unready, // Free: second squeeze or timeout; Arm: grip released (a soft tick)
        Start,   // Lift / Hold: the grip closed, the charge building
        Fire,    // Lift / Hold: done: transform (Tonitrus: its L1) now
        Short,   // Lift / Hold: let go before it took (a soft tick)
        Cooling, // Lift / Hold / Pull: squeezed during the cooldown (the deny; Pull: a dry cough)
        Cough,   // Pull: a pull that did not catch
        Catch,   // Pull: caught (held: revs; squeezed: pumps; any valid strike: lights)
        Pump,    // Pull: caught, squeezed again: revved up Settings::boom_pump more
        Die,     // Pull: idled out unrevved: it sputters out (pull again)
    };
    struct Out {
        Event event{Event::None};
        bool ready{false};    // Free / Arm: freed or armed now
        bool charging{false}; // Lift / Hold: held, building ...; Pull: caught, idling ...
        float progress{0.0f}; // ... 0..1 (Pull: revved up)
        float cooldown_s{0.0f};
        bool struck{false};       // Fire: the Boom Hammer's head struck (StrikeLights), not held
        float strike_speed{0.0f}; // ... the gap closing then (m/s)
        int pull{0};              // Cough / Catch: which pull this was (1, 2, ...)
    };

    // Every gameplay frame: `f.right.grip` the right grip (latched), `use` the weapon's
    // (GripUseOf), `family` it.
    Out Step(const Frame& f, GripUse use, std::int32_t family, const Settings& s) {
        Out out{};
        const bool grip = f.right.grip;
        const bool edge = grip && !prev_grip;
        prev_grip = grip;
        cooldown = std::max(cooldown - f.dt, 0.0f);
        if (use == GripUse::Pull && !s.boom_pull) {
            use = GripUse::Hold;
        }
        if (use != last_use || family != last_family) {
            last_use = use;
            last_family = family;
            ready = false;
            charging = false;
            gap = -1.0f;
            pulls = 0;
            rev = 0.0f;
        }
        switch (use) {
        case GripUse::Pull: {
            const bool struck = StrikeLights(family) && Struck(f);
            Engine(f, edge, grip, struck, s, out);
            break;
        }
        case GripUse::Arm:
            if (edge) {
                ready = true;
                out.event = Event::Armed;
            } else if (ready && !grip) {
                ready = false;
                out.event = Event::Unready;
            }
            break;
        case GripUse::Free:
            if (edge) {
                ready = !ready;
                ready_s = 0.0f;
                out.event = ready ? Event::Freed : Event::Unready;
            } else if (ready) {
                ready_s += f.dt;
                if (ready_s > FreeS) {
                    ready = false;
                    out.event = Event::Unready;
                }
            }
            break;
        case GripUse::Lift:
        case GripUse::Hold: {
            // (Followed every frame, the grip held or not: a strike comes in from outside.)
            const bool struck = use == GripUse::Hold && StrikeLights(family) && Struck(f);
            if (edge && cooldown > 0.0f) {
                out.event = Event::Cooling;
            } else if (edge) {
                charging = true;
                held_s = 0.0f;
                low_y = f.right.p.y;
                out.event = Event::Start;
            } else if (charging && !grip) {
                charging = false;
                out.event = Event::Short;
            }
            if (charging && (struck || Charge(f, use, family))) {
                charging = false;
                out.event = Event::Fire;
                out.struck = struck;
                out.strike_speed = struck ? closing : 0.0f;
                cooldown = CoolsDown(family) ? s.charge_cooldown_s : 0.0f;
            }
            break;
        }
        case GripUse::None:
        case GripUse::Transform:
            ready = false;
            charging = false;
            break;
        }
        out.ready = ready;
        out.charging = charging;
        out.progress = charging ? (use == GripUse::Pull ? rev : progress) : 0.0f;
        out.cooldown_s = cooldown;
        return out;
    }

    // A new random sequence of catches (the presenter: the clock), so no two sessions pull alike.
    void Seed(std::uint32_t value) {
        seed = value != 0 ? value : 0x9E3779B9u;
    }

    // The freed hand or the armed stab was used (its transform taken): back to idle.
    void Consume() {
        ready = false;
    }

    // The charge's transform never reached the game (L1 dropped): no cooldown for it.
    void CancelCooldown() {
        cooldown = 0.0f;
    }

    // Idle, the right grip as it is now (a grip already held is no squeeze); a cooldown runs on.
    void Reset(bool grip_now) {
        const float c = cooldown;
        const std::uint32_t sd = seed;
        *this = RightGrip{};
        cooldown = c;
        seed = sd;
        prev_grip = grip_now;
    }

private:
    // Cold: each squeeze is a starter CHUFF with a short strike window. Once caught
    // (`charging`): any valid strike lights it, or hold to rev over boom_rev_s. Let go,
    // the rev runs back down and after boom_idle_s it dies.
    void Engine(const Frame& f, bool edge, bool grip, bool struck, const Settings& s, Out& out) {
        since_pull += f.dt;
        if (cooldown > 0.0f) {
            charging = false;
            pulls = 0;
            rev = 0.0f;
            if (edge) {
                out.event = Event::Cooling;
            }
            return;
        }
        if (!charging) {
            if (pulls > 0 && since_pull > s.boom_pull_gap_s) {
                pulls = 0; // it sagged: start over
            }
            if (edge) {
                if (pulls == 0) { // a new start: which pull catches
                    const int lo = std::max(1, s.boom_pulls_min);
                    const int hi = std::max(lo, s.boom_pulls_max);
                    seed = seed * 1664525u + 1013904223u;
                    catch_on = lo + static_cast<int>((seed >> 16) % static_cast<std::uint32_t>(hi - lo + 1));
                }
                ++pulls;
                since_pull = 0.0f;
                out.pull = pulls;
            }
            // Also accept a strike coincident with the squeeze. An expired strike
            // cannot manufacture another CHUFF and grant itself a fresh window.
            if (struck && pulls > 0 && since_pull <= BoomStrikeWindowS) {
                Ignite(true, s, out);
                return;
            }
            if (!edge) {
                return;
            }
            if (pulls < catch_on) {
                out.event = Event::Cough;
                return;
            }
            pulls = 0;
            charging = true;
            idle = 0.0f;
            rev = 0.0f;
            out.event = Event::Catch;
            return;
        }
        if (edge) { // squeezed again: pumped up (mashing the grip lights it too)
            rev = std::min(rev + s.boom_pump, 1.0f);
            out.event = Event::Pump;
        }
        if (grip) {
            rev = std::min(rev + f.dt / s.boom_rev_s, 1.0f);
            idle = 0.0f;
        } else {
            rev = std::max(rev - f.dt / s.boom_rev_s, 0.0f);
            idle += f.dt;
        }
        if (struck || rev >= 1.0f) {
            Ignite(struck, s, out);
        } else if (idle >= s.boom_idle_s) {
            charging = false;
            rev = 0.0f;
            out.event = Event::Die;
        }
    }

    void Ignite(bool struck, const Settings& s, Out& out) {
        charging = false;
        pulls = 0;
        rev = 0.0f;
        out.event = Event::Fire;
        out.struck = struck;
        out.strike_speed = struck ? closing : 0.0f;
        cooldown = s.boom_cooldown_s;
    }

    // The left hand's line (its gun, its torch) striking the weapon's head this frame: in from
    // outside FurnaceR, the gap closing at StrikeSpeed at least (smoothed over a few frames).
    bool Struck(const Frame& f) {
        if (!f.left.tracked || !f.right.valid || Body::LengthSq(f.axis) < 0.5f || f.dt <= 0.0f) {
            gap = -1.0f; // (no strike measured across a gap)
            closing = 0.0f;
            return false;
        }
        const float reach = Body::LengthSq(f.left_axis) > 0.5f ? f.left_reach : 0.0f;
        const float now = SegmentGap(f.left.p, f.left.p + f.left_axis * reach,
                                     f.right.p + f.axis * FurnaceFromM,
                                     f.right.p + f.axis * FurnaceToM);
        const bool had = gap >= 0.0f;
        closing = had ? closing + 0.5f * ((gap - now) / f.dt - closing) : 0.0f;
        const bool hit = had && gap > FurnaceR && now <= FurnaceR && closing >= StrikeSpeed;
        gap = now;
        return hit;
    }

    // The charge held a frame: true when it is done.
    bool Charge(const Frame& f, GripUse use, std::int32_t family) {
        held_s += f.dt;
        if (use == GripUse::Hold) {
            progress = std::clamp(held_s / HoldSOf(family), 0.0f, 1.0f);
            return progress >= 1.0f;
        }
        if (!f.right.valid) {
            return false; // (where it was: no lift measured across a gap)
        }
        low_y = std::min(low_y, f.right.p.y);
        const float rise = f.right.p.y - low_y;
        // The build tracks the whole way to the goal (LiftM, and over the eyes), not only LiftM.
        progress = std::clamp(rise / std::max(LiftM, f.head.y + LiftAboveM - low_y), 0.0f, 1.0f);
        const bool up = Body::LengthSq(f.axis) < 0.5f || f.axis.y >= LiftUp;
        return rise >= LiftM && f.right.p.y >= f.head.y + LiftAboveM && up;
    }

    bool prev_grip{false};
    GripUse last_use{GripUse::Transform};
    std::int32_t last_family{-1};
    bool ready{false};
    float ready_s{0.0f};
    bool charging{false};
    float held_s{0.0f};
    float low_y{0.0f};
    float progress{0.0f};
    float cooldown{0.0f};
    float gap{-1.0f};    // Struck: the left hand's line to the head last frame (-1: none)
    float closing{0.0f}; // ... how fast it closes (m/s)
    // Engine (GripUse::Pull): the pulls of this start, the one that catches, since the last;
    // caught: unrevved for `idle`, revved up `rev` (0..1).
    int pulls{0};
    int catch_on{1};
    float since_pull{0.0f};
    float idle{0.0f};
    float rev{0.0f};
    std::uint32_t seed{0x9E3779B9u};
};

// GrabFamily: form B lasts while both hands hold the handle. Letting go (the grip, the hands apart,
// their line twisted) turns it back once the hand has stayed off ReleaseGraceS, counted only
// outside a swing or a transform still running (`busy`: a big two-handed swing that parts the hands
// gets its whole grace after it, to take the handle again); a grab again before then keeps it.
constexpr float ReleaseGraceS = 0.35f;

struct HoldForm {
    bool pending{false};
    float off_s{0.0f};

    // Every gameplay frame; true on the frame the weapon should go back to form A.
    bool Step(float dt, bool form_b, bool engaged, bool let_go, bool busy) {
        if (engaged) {
            pending = false;
            return false;
        }
        if (let_go) {
            pending = true;
            off_s = 0.0f;
        }
        if (!pending || busy) {
            return false;
        }
        off_s += dt;
        if (off_s < ReleaseGraceS) {
            return false;
        }
        pending = false;
        return form_b;
    }
};

// A blast (ShotWeapon::charge_s > 0: the Rifle Spear's, the Stake Driver's): the trigger held
// charges it; a verified stake strike or release once charged fires. Let go before: a soft tick.
// A squeeze
// during the cooldown after a blast, or while a blast is not allowed (`allowed`: swings held
// back, the vial at the wrist), is refused.
class Blast {
public:
    enum class Event : std::uint8_t { None, Start, Full, Fire, Short, Cooling, Refused };
    struct Out {
        Event event{Event::None};
        bool charging{false};
        float progress{0.0f};
    };

    Out Step(bool trigger, bool allowed, float dt, float charge_s, float cooldown_s,
             bool strike = false) {
        Out out{};
        const bool edge = trigger && !prev;
        prev = trigger;
        cooldown = std::max(cooldown - dt, 0.0f);
        if (edge && !allowed) {
            out.event = Event::Refused;
        } else if (edge && cooldown > 0.0f) {
            out.event = Event::Cooling;
        } else if (edge) {
            charging = true;
            full = false;
            held_s = 0.0f;
            out.event = Event::Start;
        } else if (charging && !allowed) {
            charging = false; // (silently: the game took the moment)
        } else if (charging && trigger) {
            held_s += dt;
            if (!full && held_s >= charge_s) {
                full = true;
                out.event = Event::Full;
            }
            if (full && strike) {
                charging = false;
                cooldown = cooldown_s;
                out.event = Event::Fire;
            }
        } else if (charging) {
            charging = false;
            out.event = full ? Event::Fire : Event::Short;
            if (full) {
                cooldown = cooldown_s;
            }
        }
        out.charging = charging;
        out.progress = charging && charge_s > 0.0f ? std::clamp(held_s / charge_s, 0.0f, 1.0f)
                                                   : 0.0f;
        return out;
    }

    // Not charging, the trigger as it is now; a cooldown runs on.
    void Reset(bool trigger_now) {
        charging = false;
        full = false;
        prev = trigger_now;
    }

    // A frame out of the blast's form: not charging, the cooldown still running down (a form
    // left and taken again later finds it over, as the game's own is).
    void Idle(bool trigger_now, float dt) {
        Reset(trigger_now);
        cooldown = std::max(cooldown - std::max(dt, 0.0f), 0.0f);
    }

private:
    bool prev{false};
    bool charging{false};
    bool full{false};
    float held_s{0.0f};
    float cooldown{0.0f};
};

// ---- The shield ---------------------------------------------------------------------------------

// The Wooden Shield (190) and the Loch Shield (191).
constexpr bool IsShieldFamily(std::int32_t family) {
    return family == 190 || family == 191;
}

// How big a blow on the shield was, from the hunter's guard reaction (a000_010000 .. 010999, the
// longer the bigger: 0100x0 small, 0101x0, 0102x0 large, 0103x0 and up the guard broken): 0 .. 3.
constexpr int GuardBlowSize(std::int32_t clip) {
    const std::int32_t base = clip % 1'000'000;
    if (clip < 0 || base < 10000 || base > 10999) {
        return 0;
    }
    return std::min((base - 10000) / 10, 3);
}

// Up while the left hand is held up in front of you: at least shield_m in front of the eyes,
// from the chest to just above the eyes (a hand resting in front of the belly or a high ready
// stance is no guard), across the body. Once up it takes ExitSlackM more room and ExitS out of
// it to go down (a duck, a lean, the edge of the zone never flicker it).
class ShieldRaise {
public:
    bool Up() const {
        return up;
    }
    void Reset() {
        up = false;
        dwell = 0.0f;
        lost = 0.0f;
        out_s = 0.0f;
    }
    bool Step(const Frame& f, const Settings& s) {
        if (!f.left.valid) {
            // Up through a short tracking dropout (the other hand crossing in front of it).
            lost += f.dt;
            if (lost > LostS) {
                Reset();
            }
            return up;
        }
        lost = 0.0f;
        const HeadLocal l = InHead(f.head, f.forward, f.left.p);
        const float m = up ? ExitSlackM : 0.0f;
        const bool in = l.fwd >= s.shield_m - m && l.up >= LowM - m && l.up <= 0.15f + m &&
                        l.side >= -0.45f - m && l.side <= 0.30f + m;
        if (!in) {
            out_s += f.dt;
            if (!up || out_s >= ExitS) {
                Reset();
            }
        } else {
            out_s = 0.0f;
            if (!up) {
                dwell += f.dt;
                up = dwell >= DwellS;
            }
        }
        return up;
    }

    static constexpr float DwellS = 0.10f;     // held there this long (a hand passing is no block)
    static constexpr float LowM = -0.38f;      // no lower than this below the eyes (upper chest)
    static constexpr float ExitSlackM = 0.15f; // up: this much more room ...
    static constexpr float ExitS = 0.20f;      // ... and out of it this long to go down

private:
    bool up{false};
    float dwell{0.0f};
    float lost{0.0f};
    float out_s{0.0f};
};

} // namespace VR::Tricks
