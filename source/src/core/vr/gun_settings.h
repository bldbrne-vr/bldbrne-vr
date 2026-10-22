// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

// The left-hand gun in VR (core/vr/gun_aim.h, core/vr/bloodborne_gun.h): its vr.ini keys. Parsed
// by VR::LoadSettings, which hands every key it does not know to ParseGunSetting, and written with
// the other defaults when vr.ini is created. wrist_reload is also changed by the live Weapons
// page; shot observation stays installed for the vanilla path as well.
namespace VR::Gun {

enum class Busy : std::uint8_t {
    Buffer,  // a pull while the game has the left arm fires if it is back within gun_buffer_ms
    Vanilla, // ... and otherwise becomes one press of the game's own L2
    Drop,    // ... and otherwise is dropped (Buffer and Drop are the same)
};

struct GunSettings {
    // Shots leave along the gun's visible barrel, wherever the hunter faces (the game's own aim:
    // his facing, or the lock-on point). 0 = the game's direction.
    bool aim{true};
    float headshot_multiplier{1.8f}; // confirmed left-gun head collision; 1 disables the bonus
    bool wrist_reload{true}; // live: a wrist flick after each confirmed left-weapon shot
    // direct: the bang at the trigger pull (no draw animation; pistols, Blunderbuss, Ludwig's
    // Rifle); vanilla: the left trigger is the game's L2 (its 0.3-0.5 s draw), still aimed along
    // the barrel when gun_aim=1.
    bool fire_direct{true};
    // Locked on, the share of the aiming error removed near the target (at most about 1.2 degrees;
    // 0 = none), within gun_assist_deg of it. (The tester's 27%, 2026-10-07; 0.25 before 2026-10-06,
    // 0.30 on 10-06.)
    float assist{0.27f};
    float assist_deg{8.0f};
    bool assist_unlocked{false}; // also without lock-on (off: no pull toward anything)
    // The bullet starts on the barrel's line (moved sideways from the palm, at most a few cm, never
    // forward); 0 = from the palm as the game starts it.
    bool origin_barrel{true};
    // A pull while the game has the left arm (a dodge, a hit, a weapon switch: the VR body gives
    // it back when that clip ends or another starts) waits gun_buffer_ms for the arm, then becomes
    // one press of the game's own L2, which the game fires as soon as its clip allows (aimed along
    // the barrel of the pull). buffer / drop: then nothing (a dodge's arm is often longer away).
    Busy busy{Busy::Vanilla};
    float buffer_ms{250.0f};
    float rate_scale{1.0f};    // the gun's cadence (1 = the game's own, 0.6 .. 1)
    float recoil_visual{1.0f}; // the hunter's hand kicks up at the bang (0 = off .. 1.5)
    bool haptics{true};        // trigger break, recoil thump, empty click, not-ready tick
    float recoil_gain{1.0f};   // strength of the recoil thump (0 .. 1.5)
    bool regen_pause{true};    // no stamina regen just after a shot (as the game's own shot)
};

GunSettings& GetGunSettings();

// VR::LoadSettings: true if `key` is one of ours (and it was applied).
bool ParseGunSetting(const std::string& key, const std::string& value);
// The defaults block written into a new vr.ini.
void WriteGunDefaults(std::ostream& out);
// "gun_aim=1 gun_fire=direct ..." for the install line.
std::string GunSettingsText();

} // namespace VR::Gun
