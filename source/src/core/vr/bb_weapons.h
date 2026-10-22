// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// GENERATED offline (by a tool not included here) from the 1.09 EquipParamWeapon and the
// English weapon names (item.msgbnd); do not edit by hand.
//
// Bloodborne weapon families for the VR body. A family is the EquipParamWeapon id / 100000: one
// weapon model with its Uncanny / Lost variants, upgrade levels and gems (70 = Saw Cleaver
// 7000000 .. Lost Saw Cleaver +10 7021000). The live weapon grip adjust (hold A+B) saves one
// grip per family as vr.ini weapon_grip_<key>. `hold` is the body-feel design's hold class for the
// normal (one-handed) form, logged with the weapon [U: not used for placement yet]; dmy_a /
// dmy_b / dmy_left are the body dummies the in-hand part attaches to (right form A / B, left
// form A; -1 none).
namespace VR::Body::Weapons {

enum class Hold : std::uint8_t { Blade, Haft, Hand, Gun, Shield, Torch };

inline const char* HoldName(Hold hold) {
    switch (hold) {
    case Hold::Blade:
        return "blade";
    case Hold::Haft:
        return "haft";
    case Hold::Hand:
        return "hand";
    case Hold::Gun:
        return "gun";
    case Hold::Shield:
        return "shield";
    case Hold::Torch:
        return "torch";
    }
    return "?";
}

struct Family {
    std::int32_t family;
    std::string_view key; // vr.ini weapon_grip_<key>
    std::string_view name;
    Hold hold;
    std::int16_t dmy_a;
    std::int16_t dmy_b;
    std::int16_t dmy_left;
    std::int32_t variation; // behaviorVariationId
};

inline constexpr std::array<Family, 48> Families = {{
    {9, "weapon_9", "weapon 9", Hold::Blade, 300, -1, -1, 90},
    {10, "bare_fists", "Bare Fists", Hold::Hand, 304, 324, 344, 100},
    {20, "chikage", "Chikage", Hold::Blade, 300, -1, -1, 200},
    {40, "blade_of_mercy", "Blade of Mercy", Hold::Blade, 300, -1, -1, 400},
    {50, "hunter_axe", "Hunter Axe", Hold::Haft, 300, -1, -1, 500},
    {51, "burial_blade", "Burial Blade", Hold::Blade, 300, -1, -1, 510},
    {60, "hunter_blunderbuss", "Hunter Blunderbuss", Hold::Gun, -1, -1, 345, 600},
    {61, "ludwigs_rifle", "Ludwig's Rifle", Hold::Gun, -1, -1, 345, 610},
    {70, "saw_cleaver", "Saw Cleaver", Hold::Blade, 300, -1, -1, 700},
    {71, "saw_spear", "Saw Spear", Hold::Blade, 300, -1, -1, 710},
    {80, "kirkhammer", "Kirkhammer", Hold::Blade, 300, -1, -1, 800},
    {81, "ludwigs_holy_blade", "Ludwig's Holy Blade", Hold::Blade, 300, -1, -1, 810},
    {90, "beast_claw", "Beast Claw", Hold::Hand, 304, -1, -1, 900},
    {100, "rifle_spear", "Rifle Spear", Hold::Blade, 300, -1, -1, 1000},
    {101, "reiterpallasch", "Reiterpallasch", Hold::Blade, 300, -1, -1, 1010},
    {110, "stake_driver", "Stake Driver", Hold::Hand, 309, -1, -1, 1100},
    {120, "logarius_wheel", "Logarius' Wheel", Hold::Haft, 306, -1, -1, 1200},
    {130, "tonitrus", "Tonitrus", Hold::Haft, 300, 320, -1, 1300},
    {140, "hunter_pistol", "Hunter Pistol", Hold::Gun, -1, -1, 347, 1400},
    {141, "evelyn", "Evelyn", Hold::Gun, -1, -1, 347, 1400},
    {142, "repeating_pistol", "Repeating Pistol", Hold::Gun, -1, -1, 347, 1410},
    {150, "cannon", "Cannon", Hold::Gun, -1, -1, 348, 1500},
    {180, "rosmarinus", "Rosmarinus", Hold::Gun, -1, -1, 347, 1800},
    {181, "flamesprayer", "Flamesprayer", Hold::Gun, -1, -1, 347, 1810},
    {190, "wooden_shield", "Wooden Shield", Hold::Shield, -1, -1, 341, 1900},
    {191, "loch_shield", "Loch Shield", Hold::Shield, -1, -1, 341, 1900},
    {200, "hunters_torch", "Hunter's Torch", Hold::Torch, -1, -1, 340, 2000},
    {201, "torch", "Torch", Hold::Torch, -1, -1, 340, 2000},
    {220, "threaded_cane", "Threaded Cane", Hold::Blade, 300, -1, -1, 2200},
    {230, "beasthunter_saif", "Beasthunter Saif", Hold::Blade, 300, -1, -1, 2300},
    {240, "beast_cutter", "Beast Cutter", Hold::Blade, 300, -1, -1, 2400},
    {250, "amygdalan_arm", "Amygdalan Arm", Hold::Haft, 300, -1, -1, 2500},
    {260, "holy_moonlight_sword", "Holy Moonlight Sword", Hold::Blade, 300, -1, -1, 2600},
    {270, "rakuyo", "Rakuyo", Hold::Blade, 300, -1, -1, 2700},
    {280, "boom_hammer", "Boom Hammer", Hold::Haft, 300, -1, -1, 2800},
    {290, "bloodletter", "Bloodletter", Hold::Haft, 300, -1, -1, 2900},
    {300, "church_pick", "Church Pick", Hold::Blade, 300, -1, -1, 3000},
    {310, "whirligig_saw", "Whirligig Saw", Hold::Haft, 300, -1, -1, 3100},
    {320, "simons_bowblade", "Simon's Bowblade", Hold::Blade, 300, -1, -1, 3200},
    {330, "gatling_gun", "Gatling Gun", Hold::Gun, -1, -1, 348, 3300},
    {340, "fist_of_gratia", "Fist of Gratia", Hold::Hand, -1, -1, 349, 3400},
    {350, "church_cannon", "Church Cannon", Hold::Gun, -1, -1, 350, 3500},
    {360, "piercing_rifle", "Piercing Rifle", Hold::Gun, -1, -1, 345, 3600},
    {370, "weapon_370", "weapon 370", Hold::Hand, 304, -1, -1, 3700},
    {380, "kos_parasite", "Kos Parasite", Hold::Hand, 304, -1, -1, 101},
    {390, "weapon_390", "weapon 390", Hold::Hand, 304, -1, -1, 3700},
    {400, "weapon_400", "weapon 400", Hold::Hand, 304, -1, -1, 3800},
    {410, "weapon_410", "weapon 410", Hold::Hand, 304, -1, -1, 3800},
}};

// The family of an EquipParamWeapon id, or -1.
constexpr std::int32_t FamilyOf(std::int32_t weapon_id) {
    return weapon_id >= 0 ? weapon_id / 100000 : -1;
}

inline const Family* FindFamily(std::int32_t family) {
    for (const Family& f : Families) {
        if (f.family == family) {
            return &f;
        }
    }
    return nullptr;
}

inline const Family* FindKey(std::string_view key) {
    for (const Family& f : Families) {
        if (f.key == key) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace VR::Body::Weapons
