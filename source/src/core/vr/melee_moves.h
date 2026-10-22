// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// GENERATED offline (by a tool not included here) from the 1.09 EquipParamWeapon, BehaviorParam_PC and
// AtkParam_Pc; do not edit by hand.
//
// Physical melee (core/vr/melee_window.h): the game's own attack rows a swing opens, per right-hand
// weapon (its behaviour variation: BehaviorParam_PC id = 100000000 + variation * 1000 + judge).
// Each weapon has six moves: light / heavy / charged in form A (judges 0, 150, 170) and in form B
// (400, or the R1 chain's next hit 410-440; heavy 550, 910 or 500; charged 570 or 930). A move is
// listed only when its AtkParam hits with a part of the weapon in that form (hit part 0 on dummy
// group 10 in form A, 11-13 in form B) that the RIGHT hand holds in that form (EquipParamWeapon
// dmypolyId_Slot<part>RightForm<A|B> a right-hand dummy 300-339: the swing is the right
// controller's, so never the blade in the left hand, as Rakuyo's and the Beast Claw's form-B R2,
// the Kos Parasite's form-B R1, nor a part on his back) and has a real motion value; an empty move
// ({}) has no physical swing (the game's own button then, or the next lighter move). Left out on
// purpose: charged rows weaker than the light one (Simon's Bowblade's bow shot). The Threaded
// Cane's and the Beast Cutter's form B (whips) are empty here: their rows are in
// core/vr/whip_moves.h, on the chain the physical whip simulates (whip=physical, the default:
// core/vr/bloodborne_whip.h WhipMovesFor); without it (whip=canned) their segments would trail the
// hand without the weapon's own attack clip, so they keep the game's R1 / R2.
// Stamina and motion value as BehaviorParam.stamina and AtkParam.atkPhysCorrection; radius and
// hit stop as AtkParam hit0_Radius and hitStopTime. The swing sound and the blade trail are the
// weapon's own (offline analysis): from the clip of the weapon's TAE group whose
// InvokeAttackBehavior uses the row's judge, its PlaySound_CenterBody (type 1, not a 4xxx voice)
// closest before the hit and its SpawnFFX_Blade (FFX, base and tip dummies; 3-digit ids on the
// row's part group; a trail on another part, the Blade of Mercy's form-B R2 on its left blade, is
// dropped); the TAE groups of the Blade of Mercy (a113), Tonitrus (a109), Ludwig's (a105 / a120),
// the Saw Spear's form A (a112) and weapon 390 (a155) as gen_paces.py matched them; -1 where the
// clip has none or no group is known (the Kos Parasite): the defaults then.
// blade / span: the weapon part's model round the capsule (offline analysis).
namespace VR::Melee {

struct MoveRow {
    std::int16_t judge{-1};
    std::int16_t stamina{0};
    std::int16_t mv{0};    // motion value (%)
    std::int32_t dmy1{-1}; // hit part 0: capsule from dmy1 to dmy2 (-1: a sphere at dmy1)
    std::int32_t dmy2{-1};
    float radius{0.0f};          // m
    float hitstop{0.0f};         // s
    bool charge{false};          // isChargeAtk (the charged back-stagger)
    std::int32_t whoosh{-1};     // the swing sound (PlaySound type 1; -1: the default)
    std::int32_t trail_ffx{-1};  // the blade trail (SpawnFFX_Blade; -1: the default)
    std::int32_t trail_base{-1}; // ... between these two dummies
    std::int32_t trail_tip{-1};
    // The blade's own size round the capsule (offline analysis, from the weapon part's
    // model in its bind pose): how far it reaches off the dmy1 -> dmy2 axis (a sphere row: from
    // dmy1), and that span (0: a sphere); 0 / 0 when not known. For the radius fit
    // (core/vr/melee_window.h SwingRadius), while the part in the hand has that span.
    float blade{0.0f}; // m
    float span{0.0f};  // m

    constexpr bool Valid() const {
        return judge >= 0;
    }
    // The weapon dummy group of the part it hits with (10 = form A, 11..13 = form B parts).
    constexpr std::int32_t Group() const {
        return dmy1 / 1000;
    }
};

// rows: light A, heavy A, charged A, light B, heavy B, charged B.
struct WeaponMoves {
    std::int32_t variation;
    std::string_view name;
    std::array<MoveRow, 6> rows;
};

// clang-format off
inline constexpr std::array<WeaponMoves, 28> Moves = {{
    {101, "Kos Parasite",
     {{{0, 15, 100, 10100, -1, 0.400f, 0.060f, false, -1, -1, -1, -1, 0.000f, 0.000f}, {150, 30, 125, 10100, -1, 0.400f, 0.090f, false, -1, -1, -1, -1, 0.000f, 0.000f}, {170, 45, 145, 10100, -1, 0.400f, 0.120f, true, -1, -1, -1, -1, 0.000f, 0.000f},
       {}, {}, {}}}},
    {200, "Chikage",
     {{{0, 23, 100, 10100, 10110, 0.350f, 0.090f, false, -1, 250, 10110, 10100, 0.042f, 0.628f}, {150, 40, 127, 10100, 10110, 0.300f, 0.090f, false, -1, -1, -1, -1, 0.042f, 0.628f}, {170, 60, 185, 10100, 10110, 0.300f, 0.120f, true, -1, -1, -1, -1, 0.042f, 0.628f},
       {400, 25, 95, 12100, 12110, 0.350f, 0.090f, false, -1, 279, 12110, 12100, 0.076f, 0.675f}, {910, 55, 150, 12100, 12110, 0.350f, 0.120f, false, 114426200, 279, 12110, 12100, 0.076f, 0.675f}, {930, 60, 225, 12100, 12110, 0.350f, 0.120f, true, 114426200, 279, 12110, 12100, 0.076f, 0.675f}}}},
    {400, "Blade of Mercy",
     {{{0, 15, 100, 10100, 10110, 0.300f, 0.060f, false, 5000, 250, 10120, 10100, 0.079f, 0.440f}, {150, 30, 125, 10100, 10110, 0.300f, 0.090f, false, 770, 250, 10120, 10100, 0.079f, 0.440f}, {170, 50, 200, 10100, 10110, 0.300f, 0.120f, true, 770, 250, 10120, 10100, 0.079f, 0.440f},
       {400, 11, 90, 12100, 12110, 0.300f, 0.060f, false, 5000, 253, 12120, 12100, 0.079f, 0.382f}, {500, 45, 150, 12100, 12110, 0.300f, 0.090f, false, 770, -1, -1, -1, 0.079f, 0.382f}, {}}}},
    {500, "Hunter Axe",
     {{{0, 25, 100, 10100, -1, 0.500f, 0.090f, false, 760, 251, 10100, 10120, 0.592f, 0.000f}, {150, 45, 135, 10100, -1, 0.500f, 0.120f, false, 760, 251, 10100, 10120, 0.592f, 0.000f}, {170, 75, 200, 10100, -1, 0.500f, 0.120f, true, 760, 251, 10100, 10120, 0.592f, 0.000f},
       {400, 27, 105, 12100, 12130, 0.500f, 0.090f, false, 761, 257, 12120, 12100, 0.330f, 0.261f}, {910, 50, 137, 12100, 12130, 0.500f, 0.120f, false, 761, 257, 12120, 12100, 0.330f, 0.261f}, {930, 100, 150, 12100, 12130, 0.500f, 0.120f, true, 611, 257, 12120, 12100, 0.330f, 0.261f}}}},
    {510, "Burial Blade",
     {{{0, 23, 100, 10100, 10110, 0.450f, 0.090f, false, 760, 250, 10100, 10120, 0.202f, 0.848f}, {150, 45, 122, 10100, 10110, 0.450f, 0.090f, false, 760, 250, 10100, 10110, 0.202f, 0.848f}, {170, 65, 170, 10100, 10110, 0.450f, 0.120f, true, 760, 250, 10100, 10110, 0.202f, 0.848f},
       {400, 28, 110, 12100, 12130, 0.400f, 0.090f, false, 611, 253, 12100, 12130, 1.000f, 0.952f}, {910, 40, 145, 12100, 12130, 0.400f, 0.120f, false, 764, 253, 12100, 12130, 1.000f, 0.952f}, {930, 100, 190, 12100, 12130, 0.400f, 0.120f, true, 760, 253, 12100, 12130, 1.000f, 0.952f}}}},
    {700, "Saw Cleaver",
     {{{0, 20, 100, 10100, 10110, 0.400f, 0.090f, false, 610, 250, 10110, 10100, 0.276f, 0.497f}, {150, 35, 120, 10100, 10110, 0.400f, 0.090f, false, 610, 250, 10120, 10100, 0.276f, 0.497f}, {170, 50, 190, 10100, 10110, 0.400f, 0.120f, true, 610, 250, 10120, 10100, 0.276f, 0.497f},
       {400, 24, 97, 12100, 12120, 0.400f, 0.090f, false, 611, 250, 12101, 12110, 0.498f, 0.165f}, {550, 38, 125, 12100, 12120, 0.400f, 0.090f, false, 611, 250, 12101, 12110, 0.498f, 0.165f}, {570, 50, 170, 12100, 12120, 0.400f, 0.120f, true, 611, 250, 12101, 12110, 0.498f, 0.165f}}}},
    {710, "Saw Spear",
     {{{0, 20, 100, 10100, 10110, 0.400f, 0.090f, false, 610, 250, 10110, 10100, 0.217f, 0.896f}, {150, 35, 120, 10100, 10110, 0.550f, 0.090f, false, 610, 250, 10120, 10100, 0.217f, 0.896f}, {170, 50, 190, 10100, 10110, 0.400f, 0.120f, true, 610, 250, 10120, 10100, 0.217f, 0.896f},
       {400, 25, 108, 12100, 12120, 0.400f, 0.090f, false, 611, 250, 12100, 12110, 0.283f, 0.464f}, {550, 35, 133, 12100, 12120, 0.400f, 0.090f, false, 611, 250, 12100, 12110, 0.283f, 0.464f}, {570, 50, 173, 12100, 12120, 0.400f, 0.120f, true, 611, -1, -1, -1, 0.283f, 0.464f}}}},
    {800, "Kirkhammer",
     {{{0, 16, 70, 10100, 10110, 0.400f, 0.090f, false, 783, 253, 10100, 10110, 0.021f, 0.527f}, {150, 55, 100, 10100, 10110, 0.400f, 0.090f, false, 783, -1, -1, -1, 0.021f, 0.527f}, {170, 75, 120, 10100, 10110, 0.400f, 0.120f, true, 783, -1, -1, -1, 0.021f, 0.527f},
       {400, 30, 100, 12100, 12122, 0.200f, 0.120f, false, 761, -1, -1, -1, 0.325f, 1.300f}, {910, 40, 125, 12100, 12122, 0.200f, 0.090f, false, 761, -1, -1, -1, 0.325f, 1.300f}, {930, 72, 230, 12100, 12122, 0.200f, 0.120f, true, 761, -1, -1, -1, 0.325f, 1.300f}}}},
    {810, "Ludwig's Holy Blade",
     {{{0, 16, 70, 10100, 10110, 0.400f, 0.090f, false, 783, 253, 10100, 10110, 0.021f, 0.634f}, {150, 55, 100, 10100, 10110, 0.400f, 0.090f, false, 783, -1, -1, -1, 0.021f, 0.634f}, {170, 75, 120, 10100, 10110, 0.400f, 0.120f, true, 783, -1, -1, -1, 0.021f, 0.634f},
       {400, 36, 100, 12100, 12110, 0.500f, 0.090f, false, 810, 252, 12101, 12121, 0.099f, 0.988f}, {910, 50, 125, 12100, 12110, 0.500f, 0.120f, false, 810, -1, -1, -1, 0.099f, 0.988f}, {930, 75, 220, 12100, 12110, 0.500f, 0.120f, true, 810, -1, -1, -1, 0.099f, 0.988f}}}},
    {900, "Beast Claw",
     {{{0, 15, 100, 10100, 10110, 0.400f, 0.060f, false, 610, -1, -1, -1, 0.150f, 0.255f}, {150, 35, 125, 10100, 10110, 0.400f, 0.090f, false, 610, -1, -1, -1, 0.150f, 0.255f}, {170, 50, 180, 10100, 10110, 0.400f, 0.120f, true, 610, -1, -1, -1, 0.150f, 0.255f},
       {400, 14, 90, 11100, 11110, 0.400f, 0.060f, false, 760, -1, -1, -1, 0.156f, 0.255f}, {500, 40, 120, 11100, 11110, 0.400f, 0.090f, false, 761, -1, -1, -1, 0.156f, 0.255f}, {}}}},
    {1000, "Rifle Spear",
     {{{0, 18, 95, 10100, 10120, 0.250f, 0.090f, false, 610, -1, -1, -1, 0.080f, 1.573f}, {150, 40, 135, 10100, 10120, 0.250f, 0.090f, false, 610, -1, -1, -1, 0.080f, 1.573f}, {170, 55, 195, 10100, 10120, 0.250f, 0.120f, true, 610, -1, -1, -1, 0.080f, 1.573f},
       {400, 20, 95, 12100, 12120, 0.300f, 0.090f, false, 760, 253, 12100, 12130, 0.080f, 1.573f}, {}, {}}}},
    {1010, "Reiterpallasch",
     {{{0, 18, 100, 10100, 10110, 0.300f, 0.060f, false, 801, -1, -1, -1, 0.057f, 0.605f}, {150, 35, 125, 10100, 10110, 0.300f, 0.090f, false, 800, 253, 10100, 10130, 0.057f, 0.605f}, {170, 50, 190, 10100, 10110, 0.300f, 0.120f, true, 801, 253, 10100, 10130, 0.057f, 0.605f},
       {400, 16, 75, 12100, 12130, 0.300f, 0.060f, false, 610, 253, 12100, 12130, 0.064f, 0.672f}, {}, {}}}},
    {1100, "Stake Driver",
     {{{0, 19, 100, 10100, 10110, 0.400f, 0.090f, false, 610, 253, 10100, 10110, 0.130f, 0.464f}, {150, 45, 135, 10100, 10110, 0.400f, 0.090f, false, 611, 253, 10100, 10110, 0.130f, 0.464f}, {170, 65, 190, 10100, 10110, 0.400f, 0.120f, true, 5400, 253, 10100, 10110, 0.130f, 0.464f},
       {400, 15, 97, 11100, 11110, 0.400f, 0.090f, false, 5400, -1, -1, -1, 0.130f, 0.464f}, {910, 55, 137, 11110, 11206, 0.400f, 0.120f, false, 8511, -1, -1, -1, 0.130f, 0.426f}, {}}}},
    {1200, "Logarius' Wheel",
     {{{0, 30, 100, 10100, -1, 0.800f, 0.090f, false, 611, -1, -1, -1, 0.533f, 0.000f}, {150, 45, 115, 10100, -1, 0.800f, 0.120f, false, -1, -1, -1, -1, 0.533f, 0.000f}, {170, 65, 160, 10100, -1, 0.800f, 0.120f, true, 611, -1, -1, -1, 0.533f, 0.000f},
       {400, 32, 102, 12100, -1, 0.800f, 0.090f, false, 611, -1, -1, -1, 0.533f, 0.000f}, {500, 20, 117, 12100, -1, 0.800f, 0.120f, false, 851, -1, -1, -1, 0.533f, 0.000f}, {}}}},
    {1300, "Tonitrus",
     {{{0, 23, 100, 10100, -1, 0.400f, 0.090f, false, -1, 250, 10100, 10110, 0.485f, 0.000f}, {150, 37, 120, 10100, -1, 0.400f, 0.090f, false, -1, 250, 10100, 10110, 0.485f, 0.000f}, {170, 55, 190, 10100, -1, 0.400f, 0.120f, true, -1, 250, 10100, 10110, 0.485f, 0.000f},
       {}, {}, {}}}},
    {2200, "Threaded Cane",
     {{{0, 17, 100, 10100, 10110, 0.300f, 0.090f, false, 790, 250, 10100, 10120, 0.019f, 0.552f}, {150, 30, 135, 10100, 10110, 0.300f, 0.090f, false, 792, -1, -1, -1, 0.019f, 0.552f}, {170, 45, 190, 10100, 10110, 0.300f, 0.120f, true, 792, -1, -1, -1, 0.019f, 0.552f},
       {}, {}, {}}}},
    {2300, "Beasthunter Saif",
     {{{0, 30, 110, 10100, 10120, 0.350f, 0.090f, false, 761, 271, 10120, 10100, 0.294f, 0.308f}, {150, 40, 145, 10100, 10120, 0.350f, 0.090f, false, 761, 271, 10120, 10100, 0.294f, 0.308f}, {170, 60, 180, 10100, 10120, 0.350f, 0.120f, true, 761, 271, 10120, 10100, 0.294f, 0.308f},
       {400, 20, 95, 12100, 12110, 0.550f, 0.090f, false, 610, 251, 12130, 12122, 0.241f, 0.856f}, {550, 35, 130, 12100, 12110, 0.400f, 0.090f, false, 610, 251, 12130, 12122, 0.241f, 0.856f}, {570, 50, 160, 12100, 12110, 0.500f, 0.120f, true, 610, 251, 12130, 12122, 0.241f, 0.856f}}}},
    {2400, "Beast Cutter",
     {{{0, 25, 100, 10100, 10122, 0.300f, 0.090f, false, 890, 250, 10100, 10120, 0.230f, 1.050f}, {150, 40, 120, 10100, 10122, 0.300f, 0.120f, false, 890, 250, 10100, 10120, 0.230f, 1.050f}, {170, 70, 190, 10100, 10122, 0.300f, 0.120f, true, 890, 250, 10100, 10120, 0.230f, 1.050f},
       {}, {}, {}}}},
    {2500, "Amygdalan Arm",
     {{{0, 25, 100, 10100, 10110, 0.400f, 0.090f, false, 920, -1, -1, -1, 0.681f, 1.794f}, {150, 40, 130, 10100, 10110, 0.400f, 0.120f, false, 920, -1, -1, -1, 0.681f, 1.794f}, {170, 60, 180, 10100, 10110, 0.400f, 0.120f, true, 920, -1, -1, -1, 0.681f, 1.794f},
       {400, 25, 95, 12100, 12130, 0.400f, 0.090f, false, 927, -1, -1, -1, 0.391f, 1.029f}, {550, 45, 120, 12100, 12130, 0.400f, 0.120f, false, 924, -1, -1, -1, 0.391f, 1.029f}, {570, 70, 170, 12100, 12130, 0.400f, 0.120f, true, 927, -1, -1, -1, 0.391f, 1.029f}}}},
    {2600, "Holy Moonlight Sword",
     {{{0, 25, 100, 10100, 10110, 0.400f, 0.090f, false, 783, 250, 10100, 10120, 0.049f, 0.896f}, {150, 40, 130, 10100, 10110, 0.400f, 0.120f, false, 783, -1, -1, -1, 0.049f, 0.896f}, {170, 60, 180, 10100, 10110, 0.400f, 0.120f, true, 783, -1, -1, -1, 0.049f, 0.896f},
       {400, 35, 100, 12100, 12110, 0.400f, 0.090f, false, 880, 266, 12100, 12120, 0.082f, 0.988f}, {550, 50, 120, 12100, 12110, 0.400f, 0.120f, false, 880, 266, 12100, 12120, 0.082f, 0.988f}, {570, 75, 140, 12100, 12110, 0.400f, 0.120f, true, 880, 266, 12100, 12120, 0.082f, 0.988f}}}},
    {2700, "Rakuyo",
     {{{0, 23, 100, 10100, 10110, 0.400f, 0.090f, false, 783, 250, 10100, 10120, 0.030f, 0.689f}, {150, 45, 135, 10100, 10110, 0.400f, 0.120f, false, 783, 250, 10100, 10120, 0.030f, 0.689f}, {170, 60, 170, 10100, 10110, 0.400f, 0.120f, true, 783, 250, 10100, 10120, 0.030f, 0.689f},
       {400, 23, 100, 12100, 12110, 0.400f, 0.090f, false, 783, 250, 12100, 12120, 0.027f, 0.652f}, {}, {}}}},
    {2800, "Boom Hammer",
     {{{0, 25, 100, 10100, 10110, 0.400f, 0.090f, false, 870, 250, 10110, 10100, 0.586f, 0.739f}, {150, 45, 135, 10100, 10110, 0.400f, 0.120f, false, 870, 250, 10110, 10100, 0.586f, 0.739f}, {170, 70, 190, 10100, 10110, 0.400f, 0.120f, true, 870, 250, 10110, 10100, 0.586f, 0.739f},
       {400, 40, 100, 12100, 12110, 0.650f, 0.120f, false, 871, 250, 12110, 12100, 0.369f, 0.739f}, {550, 60, 130, 12100, -1, 1.000f, 0.120f, false, 872, 250, 12110, 12100, 0.597f, 0.000f}, {570, 90, 180, 12100, 12110, 0.500f, 0.120f, true, 873, 250, 12110, 12100, 0.369f, 0.739f}}}},
    {2900, "Bloodletter",
     {{{0, 23, 100, 10100, 10110, 0.500f, 0.090f, false, 610, 250, 10100, 10120, 0.241f, 0.434f}, {150, 40, 150, 10100, 10110, 0.400f, 0.120f, false, 610, 250, 10100, 10120, 0.241f, 0.434f}, {170, 60, 190, 10100, 10110, 0.400f, 0.120f, true, 610, 250, 10100, 10120, 0.241f, 0.434f},
       {400, 25, 105, 12100, -1, 0.550f, 0.090f, false, 930, 252, 12100, 12130, 0.600f, 0.000f}, {550, 45, 150, 12100, -1, 0.500f, 0.120f, false, 930, 252, 12100, 12130, 0.600f, 0.000f}, {570, 70, 200, 12100, -1, 0.500f, 0.120f, true, 930, 252, 12100, 12130, 0.600f, 0.000f}}}},
    {3000, "Church Pick",
     {{{0, 20, 95, 10100, 10110, 0.300f, 0.090f, false, 760, 254, 10100, 10130, 0.737f, 1.230f}, {150, 40, 130, 10100, 10110, 0.300f, 0.120f, false, 760, 254, 10100, 10130, 0.737f, 1.230f}, {170, 55, 170, 10100, 10110, 0.300f, 0.120f, true, 760, 254, 10100, 10130, 0.737f, 1.230f},
       {400, 25, 105, 12100, 12130, 0.400f, 0.090f, false, 764, 251, 12130, 12110, 0.248f, 0.720f}, {550, 45, 140, 12100, 12130, 0.400f, 0.120f, false, 764, 251, 12130, 12110, 0.248f, 0.720f}, {570, 70, 190, 12100, 12130, 0.400f, 0.120f, true, 763, 251, 12130, 12110, 0.248f, 0.720f}}}},
    {3100, "Whirligig Saw",
     {{{0, 18, 75, 10100, 10120, 0.400f, 0.090f, false, 931, 250, 10100, 10120, 0.098f, 0.940f}, {150, 30, 105, 10100, 10120, 0.400f, 0.120f, false, 931, 250, 10100, 10120, 0.098f, 0.940f}, {170, 45, 130, 10100, 10120, 0.400f, 0.120f, true, 931, 250, 10100, 10120, 0.098f, 0.940f},
       {400, 30, 100, 12100, -1, 0.500f, 0.120f, false, 890, 269, 12100, 12110, 0.600f, 0.000f}, {550, 55, 130, 12100, -1, 0.500f, 0.120f, false, 890, 269, 12100, 12110, 0.600f, 0.000f}, {570, 80, 180, 12100, -1, 1.000f, 0.120f, true, 891, -1, -1, -1, 0.600f, 0.000f}}}},
    {3200, "Simon's Bowblade",
     {{{0, 20, 100, 10100, 10110, 0.350f, 0.090f, false, 910, -1, -1, -1, 0.707f, 0.979f}, {150, 40, 130, 10100, 10110, 0.350f, 0.090f, false, 910, -1, -1, -1, 0.707f, 0.979f}, {},
       {}, {}, {}}}},
    {3700, "weapon 390",
     {{{0, 15, 100, 10100, 10110, 0.400f, 0.090f, false, 931, -1, -1, -1, 0.150f, 0.255f}, {150, 25, 120, 10100, 10110, 0.400f, 0.120f, false, 940, -1, -1, -1, 0.150f, 0.255f}, {170, 40, 140, 10100, 10110, 0.500f, 0.120f, true, 931, -1, -1, -1, 0.150f, 0.255f},
       {410, 18, 100, 11100, 11120, 0.400f, 0.090f, false, 940, -1, -1, -1, 0.160f, 0.323f}, {500, 30, 120, 11100, 11120, 0.400f, 0.120f, false, 931, -1, -1, -1, 0.160f, 0.323f}, {}}}},
    {3800, "weapon 410",
     {{{0, 24, 200, 10100, 10110, 0.350f, 0.090f, false, 950, -1, -1, -1, 0.027f, 0.363f}, {150, 40, 235, 10100, 10110, 0.350f, 0.120f, false, 950, -1, -1, -1, 0.027f, 0.363f}, {},
       {410, 28, 210, 12100, 12110, 0.350f, 0.120f, false, 953, -1, -1, -1, 0.027f, 0.363f}, {500, 56, 250, 12100, 12110, 0.350f, 0.120f, false, 953, -1, -1, -1, 0.027f, 0.363f}, {}}}},
}};

// clang-format on

inline const WeaponMoves* FindMoves(std::int32_t variation) {
    for (const WeaponMoves& m : Moves) {
        if (m.variation == variation) {
            return &m;
        }
    }
    return nullptr;
}

// ---- The transform attacks (a flick mid-swing) -------------------------------------------------------
//
// GENERATED offline (by a tool not included here) from the same params; do not edit by hand.
//
// The game's own transform attack of each weapon (the trick attack L1 starts from an attack:
// BehaviorParam_PC judge 390 A->B, 790 B->A), which physical melee opens for the rest of a stroke
// in which the weapon-only transform flicks the weapon into its other form (core/vr/transform.h).
// Same fields and filters as Moves, and the row must hit with the part shown AFTER the form switch
// (A->B: dummy groups 11-13, B->A: group 10) and in the right hand, since the flick switches the
// form first; an empty row ({}) keeps the new form's own swing for the rest of that stroke (the
// game's B->A trick attacks of e.g. the Kirkhammer and the Threaded Cane hit with the old form
// before it folds; the Kirkhammer's A->B one with the hammer as it comes off his back; the Rifle
// Spear's, Reiterpallasch's and Simon's A->B ones are shots).
struct TransformMoveRows {
    std::int32_t variation;
    std::array<MoveRow, 2> rows; // A->B, B->A
};

// clang-format off
inline constexpr std::array<TransformMoveRows, 28> TransformMoves = {{
    {101, {{{},
           {}}}},
    {200, {{{},
           {}}}},
    {400, {{{390, 15, 120, 12100, 12110, 0.300f, 0.090f, false, 600, -1, -1, -1, 0.079f, 0.382f},
           {790, 15, 130, 10100, 10110, 0.400f, 0.090f, false, 770, 253, 10120, 10100, 0.079f, 0.440f}}}},
    {500, {{{390, 30, 110, 12100, 12130, 0.500f, 0.120f, false, 764, -1, -1, -1, 0.330f, 0.261f},
           {790, 30, 120, 10100, -1, 0.500f, 0.120f, false, 760, 251, 10120, 10100, 0.592f, 0.000f}}}},
    {510, {{{390, 25, 160, 12100, 12130, 0.400f, 0.120f, false, 761, 250, 12100, 12110, 1.000f, 0.952f},
           {}}}},
    {700, {{{390, 23, 130, 12100, 12120, 0.400f, 0.120f, false, 611, 250, 12101, 12110, 0.498f, 0.165f},
           {790, 30, 130, 10100, 10110, 0.400f, 0.120f, false, 611, 250, 10110, 10100, 0.276f, 0.497f}}}},
    {710, {{{390, 23, 130, 12100, 12120, 0.400f, 0.120f, false, 611, 250, 12101, 12110, 0.283f, 0.464f},
           {790, 30, 123, 10100, 10110, 0.400f, 0.120f, false, -1, -1, -1, -1, 0.217f, 0.896f}}}},
    {800, {{{},
           {}}}},
    {810, {{{390, 30, 105, 12100, 12110, 0.500f, 0.120f, false, 761, -1, -1, -1, 0.099f, 0.988f},
           {}}}},
    {900, {{{},
           {}}}},
    {1000, {{{},
           {790, 25, 130, 10100, 10120, 0.250f, 0.120f, false, 760, 253, 10100, 10130, 0.080f, 1.573f}}}},
    {1010, {{{},
           {790, 23, 110, 10100, 10110, 0.300f, 0.120f, false, 801, -1, -1, -1, 0.057f, 0.605f}}}},
    {1100, {{{390, 15, 110, 11100, 11110, 0.400f, 0.120f, false, 8511, -1, -1, -1, 0.130f, 0.464f},
           {790, 20, 100, 10100, 10110, 0.400f, 0.120f, false, 610, 251, 10120, 10122, 0.130f, 0.464f}}}},
    {1200, {{{},
           {}}}},
    {1300, {{{},
           {}}}},
    {2200, {{{390, 25, 120, 12100, 12130, 0.150f, 0.120f, false, 5001, 250, 12100, 12130, 0.042f, 0.292f},
           {}}}},
    {2300, {{{390, 30, 115, 12100, 12120, 0.400f, 0.120f, false, 763, 251, 12120, 12100, 0.283f, 0.302f},
           {790, 30, 110, 10100, 10110, 0.500f, 0.090f, false, 620, 271, 10120, 10100, 0.237f, 0.874f}}}},
    {2400, {{{390, 45, 140, 12100, 12130, 0.150f, 0.120f, false, 892, -1, -1, -1, 0.184f, 0.633f},
           {790, 50, 150, 10100, 10122, 0.300f, 0.120f, false, 890, 250, 10100, 10120, 0.230f, 1.050f}}}},
    {2500, {{{390, 30, 115, 12100, 12130, 0.400f, 0.120f, false, 926, -1, -1, -1, 0.391f, 1.029f},
           {790, 50, 150, 10100, 10110, 0.500f, 0.120f, false, 924, -1, -1, -1, 0.681f, 1.794f}}}},
    {2600, {{{390, 40, 120, 12100, 12110, 0.400f, 0.120f, false, 880, 266, 12100, 12120, 0.082f, 0.988f},
           {}}}},
    {2700, {{{390, 36, 130, 12100, 12110, 0.400f, 0.120f, false, 783, 250, 12100, 12120, 0.027f, 0.652f},
           {790, 50, 150, 10100, 10110, 0.400f, 0.120f, false, 783, 250, 10100, 10120, 0.030f, 0.689f}}}},
    {2800, {{{390, 25, 80, 12100, 12110, 0.600f, 0.120f, false, 602, -1, -1, -1, 0.369f, 0.739f},
           {}}}},
    {2900, {{{390, 30, 130, 12100, -1, 0.500f, 0.120f, false, 929, -1, -1, -1, 0.600f, 0.000f},
           {}}}},
    {3000, {{{390, 30, 120, 12100, 12130, 0.400f, 0.120f, false, 763, 251, 12130, 12110, 0.248f, 0.720f},
           {790, 35, 135, 10100, 10110, 0.300f, 0.120f, false, 760, 254, 10100, 10130, 0.737f, 1.230f}}}},
    {3100, {{{390, 35, 115, 12100, -1, 0.500f, 0.120f, false, 934, 269, 12100, 12110, 0.600f, 0.000f},
           {}}}},
    {3200, {{{},
           {790, 35, 135, 10100, 10110, 0.350f, 0.120f, false, 910, -1, -1, -1, 0.707f, 0.979f}}}},
    {3700, {{{},
           {}}}},
    {3800, {{{390, 28, 220, 12100, 12110, 0.550f, 0.120f, false, 952, -1, -1, -1, 0.027f, 0.363f},
           {}}}},
}};
// clang-format on

// The transform attack for a flick into form B (`to_b`) or back, or nullptr.
inline const MoveRow* FindTransformMove(std::int32_t variation, bool to_b) {
    for (const TransformMoveRows& m : TransformMoves) {
        if (m.variation == variation) {
            const MoveRow& r = m.rows[to_b ? 0 : 1];
            return r.Valid() ? &r : nullptr;
        }
    }
    return nullptr;
}

// ---- The moves' pace (the combo budget) -------------------------------------------------------------
//
// GENERATED offline (by a tool not included here) from the 1.09 TAEs; do not edit by hand.
//
// The game's own recovery of each move of Moves (core/vr/melee_window.h ComboBudget): the start of
// TAE ChrActionFlag 4 (the right-hand attack cancel, when the next R1 / R2 may start) in the clip of
// the weapon's TAE group that invokes the row's judge (030000 R1, 032620 R2, 032630 charged R2,
// 040000 / 042620 / 042630 in form B; the Saw Cleaver's first R1 a112_030000: 0.567 s). A move
// without a row of its own has its lighter move's pace (ChooseMove opens that one); 0: no physical
// swing in that form. The TAE group as gen_fx.py finds it, with the overrides gen_paces.py lists
// (the Blade of Mercy a113, Tonitrus a109, Ludwig's a105 / a120, the Saw Spear's form A a112).
struct WeaponPace {
    std::int32_t variation;
    std::array<float, 6> recover; // s: light A, heavy A, charged A, light B, heavy B, charged B
};

// clang-format off
inline constexpr std::array<WeaponPace, 28> Paces = {{
    {101, {{0.567f, 0.867f, 0.933f, 0.000f, 0.000f, 0.000f}}}, // Kos Parasite: no TAE group known (defaults: the Saw Cleaver)
    {200, {{0.633f, 0.900f, 1.100f, 0.667f, 0.900f, 1.100f}}}, // Chikage: a114
    {400, {{0.500f, 0.500f, 0.900f, 0.467f, 0.800f, 0.800f}}}, // Blade of Mercy: a113
    {500, {{0.767f, 0.900f, 1.300f, 0.967f, 0.800f, 1.567f}}}, // Hunter Axe: a103
    {510, {{0.667f, 0.933f, 1.133f, 0.800f, 0.800f, 1.267f}}}, // Burial Blade: a121
    {700, {{0.567f, 0.867f, 0.933f, 0.767f, 0.733f, 0.900f}}}, // Saw Cleaver: a112
    {710, {{0.567f, 0.867f, 0.933f, 0.800f, 0.800f, 1.100f}}}, // Saw Spear: a112 / a122
    {800, {{0.667f, 0.700f, 1.167f, 1.133f, 1.167f, 1.433f}}}, // Kirkhammer: a105
    {810, {{0.667f, 0.700f, 1.167f, 1.000f, 1.000f, 1.100f}}}, // Ludwig's Holy Blade: a105 / a120
    {900, {{0.567f, 0.833f, 1.167f, 0.533f, 1.200f, 1.200f}}}, // Beast Claw: a106
    {1000, {{0.667f, 0.733f, 1.067f, 0.700f, 0.700f, 0.700f}}}, // Rifle Spear: a104
    {1010, {{0.600f, 0.900f, 1.000f, 0.500f, 0.500f, 0.500f}}}, // Reiterpallasch: a124
    {1100, {{0.600f, 0.833f, 1.133f, 0.700f, 0.933f, 0.933f}}}, // Stake Driver: a116
    {1200, {{1.167f, 1.000f, 1.333f, 1.300f, 1.833f, 1.833f}}}, // Logarius' Wheel: a110
    {1300, {{0.733f, 0.800f, 1.200f, 0.000f, 0.000f, 0.000f}}}, // Tonitrus: a109
    {2200, {{0.567f, 0.733f, 1.067f, 0.000f, 0.000f, 0.000f}}}, // Threaded Cane: a127
    {2300, {{0.800f, 0.900f, 1.233f, 0.700f, 0.800f, 1.333f}}}, // Beasthunter Saif: a140
    {2400, {{0.767f, 0.967f, 1.300f, 0.000f, 0.000f, 0.000f}}}, // Beast Cutter: a141
    {2500, {{0.967f, 1.067f, 1.200f, 1.033f, 1.133f, 1.400f}}}, // Amygdalan Arm: a142
    {2600, {{0.967f, 0.667f, 1.000f, 0.833f, 1.033f, 0.833f}}}, // Holy Moonlight Sword: a143
    {2700, {{0.667f, 1.033f, 1.133f, 0.633f, 0.633f, 0.633f}}}, // Rakuyo: a144
    {2800, {{0.900f, 1.200f, 1.667f, 1.400f, 1.200f, 1.767f}}}, // Boom Hammer: a145
    {2900, {{0.733f, 0.967f, 1.300f, 0.933f, 0.900f, 1.267f}}}, // Bloodletter: a146
    {3000, {{0.633f, 0.833f, 1.267f, 0.800f, 0.767f, 0.967f}}}, // Church Pick: a147
    {3100, {{0.667f, 0.967f, 1.300f, 0.967f, 1.133f, 1.533f}}}, // Whirligig Saw: a148
    {3200, {{0.633f, 0.933f, 0.933f, 0.000f, 0.000f, 0.000f}}}, // Simon's Bowblade: a149
    {3700, {{0.733f, 1.000f, 1.600f, 0.600f, 1.333f, 1.333f}}}, // weapon 390: a155
    {3800, {{1.100f, 1.167f, 1.167f, 1.267f, 1.767f, 1.767f}}}, // weapon 410: a156
}};
// clang-format on

inline const WeaponPace* FindPace(std::int32_t variation) {
    for (const WeaponPace& p : Paces) {
        if (p.variation == variation) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace VR::Melee
