// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

// GENERATED offline (by a tool not included here) (the 1.09
// HKS Act_ChangeMode, the behaviour graph's ChangeGimmickNN states, a00.tae, the a00 hkx lengths
// and each weapon's own anibnd); do not edit by hand.
//
// The trick weapons' transform clips for the weapon-only transform (core/vr/transform.h): one row
// per weapon family (EquipParamWeapon id / 100000) and direction. `clip` is the player clip the
// game would play for L1 (a000_<clip>); the weapon model has a clip of the same id in its own
// anibnd when `weapon_anim` (it unfolds in the hand), else its parts just change at the switch.
// `style` is the arm style TAE event 32 SetWeaponStyle writes at `t_style` (1 = form A, 3 = form
// B two-handed, 5 = form B one-handed); `still`: the clip's lead-in in which the weapon model does
// not move (the hunter's own wind-up in vanilla; measured in the mock, 0 = not measured); `flick`:
// where a flick starts (a squeeze during a swing: the latest clip time before the switch at which
// the weapon is still within 4 cm of its rest pose, measured; at least `still`). The cues
// are the clip's own sounds (TAE 128, type 1: c<id>) and effects (TAE 96: ffx, dummy, slot; slot
// -1 = one-shot) replayed without the body; the clip's body-only events (cancel windows, blends,
// footsteps, cloth, the empty marker SpEffects 200 / 201 / 205 / 206 / 221, the gun-hide 322) are
// not. `support`: Tested = seen
// working in the mock (eye dumps and the form flags), Untested = the same mechanism with only
// cosmetic events, Vanilla = keeps the game's L1 (`why`: the clip carries gameplay events such as
// SpEffects or bullets, there is no clip, or it is not a trick weapon).
namespace VR::Transform {

enum class CueKind : std::uint8_t { Sound, Ffx };

enum class Support : std::uint8_t { Tested, Untested, Vanilla };

struct Cue {
    CueKind kind;
    float t0;       // s into the clip
    float t1;       // the event's end (a held effect lives until then)
    std::int32_t a; // Sound: the c-sound id; Ffx: the effect id
    std::int32_t b; // Ffx: the dummy poly (10xxx / 12xxx: the right weapon's part 0 / part 2)
    std::int32_t c; // Ffx: the slot (-1: one-shot)
};

struct Clip {
    std::int32_t family; // EquipParamWeapon id / 100000
    bool to_b;           // form A -> form B
    std::int32_t clip;   // a000_<clip>
    float length;        // s
    std::int32_t style;  // the arm style it sets ...
    float t_style;       // ... at this time
    float still;         // s: the weapon model does not move before this (measured; 0 = unknown)
    float flick;         // s: a flick starts here (no visible jump; >= still, <= t_style)
    bool weapon_anim;    // the weapon model has its own clip of this id
    Support support;
    const char* why; // Vanilla: why
    std::array<Cue, 8> cues;
    std::uint8_t cue_count;
};

// Generated table: keep clang-format away.
// clang-format off
inline constexpr std::array<Clip, 60> Clips = {{
    // Bare Fists, A->B (a000_19130; no weapon clip: the parts change at the switch)
    {10, true, 19130, 0.833f, 3, 0.200f, 0.000f, 0.000f, false, Support::Vanilla,
     "bare fists: not a trick weapon",
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0}}},
     1},
    // Bare Fists, B->A (a000_19280; no weapon clip: the parts change at the switch)
    {10, false, 19280, 0.833f, 1, 0.267f, 0.000f, 0.000f, false, Support::Vanilla,
     "bare fists: not a trick weapon",
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0}}},
     1},
    // Chikage, A->B (a000_19020)
    {20, true, 19020, 2.500f, 3, 0.433f, 0.000f, 0.000f, true, Support::Vanilla,
     "the clip adds SpEffect 204 (the blood coating)",
     {{{CueKind::Sound, 0.233f, 0.333f, 114426000, 0, 0},
       {CueKind::Ffx, 0.400f, 2.500f, 7107, 10120, 0},
       {CueKind::Sound, 1.333f, 1.433f, 114426200, 0, 0}}},
     3},
    // Chikage, B->A (a000_19230; no weapon clip: the parts change at the switch)
    {20, false, 19230, 1.233f, 1, 0.367f, 0.000f, 0.000f, false, Support::Vanilla,
     "the clip adds SpEffect 204 (the blood coating)",
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 4500, 0, 0},
       {CueKind::Ffx, 0.433f, 1.233f, 7108, 1, 0}}},
     3},
    // Blade of Mercy, A->B (a000_19030; no weapon clip: the parts change at the switch)
    {40, true, 19030, 1.167f, 3, 0.267f, 0.267f, 0.267f, false, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.133f, 0.233f, 4500, 0, 0},
       {CueKind::Sound, 0.200f, 0.300f, 600, 0, 0},
       {CueKind::Sound, 0.500f, 0.533f, 5910, 0, 0},
       {CueKind::Sound, 0.500f, 0.533f, 773, 0, 0},
       {CueKind::Ffx, 0.500f, 0.600f, 7102, 92, -1},
       {CueKind::Sound, 0.500f, 0.600f, 601, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 775, 0, 0}}},
     8},
    // Blade of Mercy, B->A (a000_19240; no weapon clip: the parts change at the switch)
    {40, false, 19240, 1.233f, 1, 0.600f, 0.590f, 0.593f, false, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.233f, 0.300f, 771, 0, 0},
       {CueKind::Sound, 0.233f, 0.333f, 601, 0, 0},
       {CueKind::Sound, 0.300f, 0.400f, 4500, 0, 0},
       {CueKind::Sound, 0.500f, 0.567f, 600, 0, 0},
       {CueKind::Sound, 0.567f, 0.600f, 5910, 0, 0},
       {CueKind::Sound, 0.567f, 0.600f, 773, 0, 0},
       {CueKind::Ffx, 0.567f, 0.733f, 7102, 55, 1}}},
     8},
    // Hunter Axe, A->B (a000_19040)
    {50, true, 19040, 1.233f, 3, 0.600f, 0.470f, 0.483f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.400f, 0.500f, 740, 0, 0},
       {CueKind::Ffx, 0.467f, 0.567f, 7100, 10120, -1}}},
     2},
    // Hunter Axe, B->A (a000_19250)
    {50, false, 19250, 1.400f, 1, 0.567f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.400f, 741, 0, 0},
       {CueKind::Sound, 0.400f, 0.500f, 740, 0, 0},
       {CueKind::Ffx, 0.600f, 0.700f, 7100, 10120, -1}}},
     3},
    // Burial Blade, A->B (a000_19220)
    {51, true, 19220, 1.667f, 3, 0.433f, 0.433f, 0.433f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.400f, 760, 0, 0},
       {CueKind::Sound, 0.400f, 0.433f, 741, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 620, 0, 0},
       {CueKind::Ffx, 0.467f, 0.567f, 7112, 12130, -1},
       {CueKind::Sound, 0.800f, 0.900f, 761, 0, 0}}},
     5},
    // Burial Blade, B->A (a000_19350)
    {51, false, 19350, 1.733f, 1, 0.867f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Ffx, 0.333f, 0.433f, 7100, 1, -1},
       {CueKind::Sound, 0.367f, 0.400f, 760, 0, 0},
       {CueKind::Sound, 0.400f, 0.433f, 741, 0, 0}}},
     3},
    // Saw Cleaver, A->B (a000_19000)
    {70, true, 19000, 1.167f, 5, 0.467f, 0.200f, 0.230f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.233f, 0.333f, 4800, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 602, 0, 0},
       {CueKind::Sound, 0.333f, 0.367f, 600, 0, 0},
       {CueKind::Sound, 0.367f, 0.467f, 601, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7110, 10110, -1}}},
     5},
    // Saw Cleaver, B->A (a000_19010)
    {70, false, 19010, 1.167f, 1, 0.467f, 0.000f, 0.440f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.233f, 0.333f, 4800, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 602, 0, 0},
       {CueKind::Sound, 0.333f, 0.433f, 601, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7110, 12110, -1}}},
     4},
    // Saw Spear, A->B (a000_19000)
    {71, true, 19000, 1.167f, 5, 0.467f, 0.310f, 0.318f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.233f, 0.333f, 4800, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 602, 0, 0},
       {CueKind::Sound, 0.333f, 0.367f, 600, 0, 0},
       {CueKind::Sound, 0.367f, 0.467f, 601, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7110, 10110, -1}}},
     5},
    // Saw Spear, B->A (a000_19010)
    {71, false, 19010, 1.167f, 1, 0.467f, 0.310f, 0.310f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.233f, 0.333f, 4800, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 602, 0, 0},
       {CueKind::Sound, 0.333f, 0.433f, 601, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7110, 12110, -1}}},
     4},
    // Kirkhammer, A->B (a000_19070)
    {80, true, 19070, 1.833f, 3, 0.467f, 0.460f, 0.460f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.467f, 781, 0, 0},
       {CueKind::Ffx, 0.500f, 0.600f, 7101, 12120, -1}}},
     2},
    // Kirkhammer, B->A (a000_19260)
    {80, false, 19260, 1.400f, 1, 0.467f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.467f, 781, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7101, 12120, -1}}},
     2},
    // Ludwig's Holy Blade, A->B (a000_19400; no weapon clip: the parts change at the switch)
    {81, true, 19400, 1.167f, 3, 0.467f, 0.450f, 0.457f, false, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.467f, 781, 0, 0},
       {CueKind::Ffx, 0.500f, 0.600f, 7101, 12120, -1}}},
     2},
    // Ludwig's Holy Blade, B->A (a000_19410; no weapon clip: the parts change at the switch)
    {81, false, 19410, 1.333f, 1, 0.467f, 0.460f, 0.465f, false, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.467f, 781, 0, 0},
       {CueKind::Ffx, 0.433f, 0.533f, 7101, 12120, -1}}},
     2},
    // Beast Claw, A->B (a000_19080)
    {90, true, 19080, 1.867f, 3, 0.767f, 0.750f, 0.758f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.467f, 1.867f, 7103, 3, 0},
       {CueKind::Sound, 0.567f, 0.600f, 4444, 0, 0},
       {CueKind::Sound, 0.600f, 0.700f, 830, 0, 0}}},
     4},
    // Beast Claw, B->A (a000_19090)
    {90, false, 19090, 1.400f, 1, 0.567f, 0.567f, 0.567f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.367f, 1.400f, 7103, 3, 0},
       {CueKind::Sound, 0.433f, 0.467f, 4444, 0, 0},
       {CueKind::Sound, 0.467f, 0.567f, 831, 0, 0}}},
     4},
    // Rifle Spear, A->B (a000_19120)
    {100, true, 19120, 1.400f, 3, 0.667f, 0.270f, 0.270f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 803, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 740, 0, 0},
       {CueKind::Ffx, 0.633f, 0.733f, 7105, 10130, -1}}},
     4},
    // Rifle Spear, B->A (a000_19270)
    {100, false, 19270, 1.400f, 1, 0.500f, 0.450f, 0.456f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 4500, 0, 0},
       {CueKind::Sound, 0.367f, 0.467f, 804, 0, 0},
       {CueKind::Sound, 0.467f, 0.567f, 740, 0, 0},
       {CueKind::Ffx, 0.567f, 0.667f, 7105, 10130, -1}}},
     5},
    // Reiterpallasch, A->B (a000_19360)
    {101, true, 19360, 1.500f, 5, 0.467f, 0.380f, 0.384f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 803, 0, 0},
       {CueKind::Ffx, 0.467f, 0.567f, 7105, 55, -1}}},
     3},
    // Reiterpallasch, B->A (a000_19370)
    {101, false, 19370, 1.333f, 1, 0.400f, 0.330f, 0.389f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.300f, 0.333f, 4500, 0, 0},
       {CueKind::Sound, 0.367f, 0.467f, 804, 0, 0},
       {CueKind::Ffx, 0.467f, 0.567f, 7105, 10130, -1}}},
     4},
    // Stake Driver, A->B (a000_19100)
    {110, true, 19100, 1.267f, 5, 0.500f, 0.500f, 0.500f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.433f, 0.533f, 8511, 0, 0},
       {CueKind::Ffx, 0.567f, 0.667f, 60, 1, -1}}},
     2},
    // Stake Driver, B->A (a000_19110)
    {110, false, 19110, 1.267f, 1, 0.500f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.433f, 0.533f, 8512, 0, 0},
       {CueKind::Ffx, 0.567f, 0.667f, 60, 1, -1}}},
     2},
    // Logarius' Wheel, A->B (a000_19160)
    {120, true, 19160, 1.500f, 3, 0.700f, 0.000f, 0.000f, true, Support::Vanilla,
     "the B->A clip adds SpEffect 6029",
     {{{CueKind::Sound, 0.433f, 0.467f, 5600, 0, 0},
       {CueKind::Sound, 0.433f, 0.500f, 5800, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 853, 0, 0},
       {CueKind::Sound, 0.467f, 0.567f, 852, 0, 0},
       {CueKind::Ffx, 0.667f, 0.767f, 7104, 10120, -1},
       {CueKind::Sound, 2.767f, 2.800f, 8100, 0, 0}}},
     6},
    // Logarius' Wheel, B->A (a000_19310)
    {120, false, 19310, 1.600f, 1, 0.300f, 0.000f, 0.000f, true, Support::Vanilla,
     "the B->A clip adds SpEffect 6029",
     {{{CueKind::Sound, 0.433f, 0.467f, 5600, 0, 0},
       {CueKind::Sound, 0.433f, 0.500f, 5800, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 853, 0, 0},
       {CueKind::Sound, 0.467f, 0.567f, 852, 0, 0},
       {CueKind::Ffx, 0.667f, 0.767f, 7104, 10120, -1}}},
     5},
    // Threaded Cane, A->B (a000_19150)
    {220, true, 19150, 1.167f, 5, 0.267f, 0.267f, 0.267f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.033f, 0.133f, 793, 0, 0},
       {CueKind::Sound, 0.267f, 0.367f, 5850, 0, 0},
       {CueKind::Sound, 0.367f, 0.467f, 770, 0, 0},
       {CueKind::Ffx, 0.500f, 0.833f, 7109, 12120, 0}}},
     5},
    // Threaded Cane, B->A (a000_19300)
    {220, false, 19300, 1.333f, 1, 0.500f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0},
       {CueKind::Sound, 0.033f, 0.133f, 793, 0, 0},
       {CueKind::Sound, 0.400f, 0.500f, 794, 0, 0},
       {CueKind::Ffx, 0.467f, 0.800f, 7109, 12120, 0}}},
     4},
    // Beasthunter Saif, A->B (a000_19500)
    {230, true, 19500, 1.433f, 5, 0.467f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.400f, 0.500f, 4800, 0, 0},
       {CueKind::Sound, 0.467f, 0.500f, 602, 0, 0},
       {CueKind::Sound, 0.500f, 0.600f, 975, 0, 0},
       {CueKind::Ffx, 0.633f, 0.733f, 7100, 12130, -1}}},
     4},
    // Beasthunter Saif, B->A (a000_19510)
    {230, false, 19510, 1.400f, 1, 0.467f, 0.260f, 0.260f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.433f, 0.533f, 4800, 0, 0},
       {CueKind::Sound, 0.500f, 0.533f, 602, 0, 0},
       {CueKind::Sound, 0.533f, 0.567f, 600, 0, 0},
       {CueKind::Sound, 0.567f, 0.667f, 975, 0, 0},
       {CueKind::Ffx, 0.633f, 0.733f, 7100, 10130, -1}}},
     5},
    // Beast Cutter, A->B (a000_19520)
    {240, true, 19520, 1.600f, 5, 0.267f, 0.267f, 0.267f, true, Support::Tested,
     nullptr,
     {{{CueKind::Ffx, 0.167f, 0.200f, 250110, 10100, -1},
       {CueKind::Sound, 0.200f, 0.300f, 895, 0, 0},
       {CueKind::Ffx, 0.233f, 0.267f, 250110, 10130, -1},
       {CueKind::Ffx, 0.267f, 0.300f, 250110, 12110, -1},
       {CueKind::Sound, 0.333f, 0.433f, 892, 0, 0},
       {CueKind::Sound, 0.433f, 0.533f, 896, 0, 0},
       {CueKind::Ffx, 0.700f, 1.033f, 7126, 12120, 0}}},
     7},
    // Beast Cutter, B->A (a000_19530)
    {240, false, 19530, 1.467f, 1, 0.600f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.400f, 890, 0, 0},
       {CueKind::Sound, 0.567f, 0.667f, 891, 0, 0},
       {CueKind::Ffx, 0.600f, 0.933f, 7126, 10120, 0}}},
     3},
    // Amygdalan Arm, A->B (a000_19540)
    {250, true, 19540, 1.500f, 5, 0.167f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.067f, 0.167f, 925, 0, 0},
       {CueKind::Ffx, 0.900f, 1.000f, 7101, 100, -1},
       {CueKind::Sound, 0.900f, 1.000f, 851, 0, 0},
       {CueKind::Sound, 0.900f, 1.000f, 922, 0, 0}}},
     4},
    // Amygdalan Arm, B->A (a000_19550)
    {250, false, 19550, 1.333f, 1, 0.467f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.000f, 0.100f, 925, 0, 0},
       {CueKind::Sound, 0.400f, 0.500f, 920, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 851, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 922, 0, 0},
       {CueKind::Ffx, 0.667f, 0.767f, 7101, 100, -1}}},
     5},
    // Holy Moonlight Sword, A->B (a000_19560; no weapon clip: the parts change at the switch)
    {260, true, 19560, 2.333f, 3, 0.467f, 0.467f, 0.467f, false, Support::Tested,
     nullptr,
     {{{CueKind::Ffx, 0.167f, 1.667f, 7116, 10120, 6},
       {CueKind::Ffx, 0.467f, 1.667f, 7117, 12120, 7}}},
     2},
    // Holy Moonlight Sword, B->A (a000_19570; no weapon clip: the parts change at the switch)
    {260, false, 19570, 1.667f, 1, 0.467f, 0.450f, 0.456f, false, Support::Tested,
     nullptr,
     {{{CueKind::Ffx, 0.300f, 1.233f, 7118, 10120, 6},
       {CueKind::Sound, 0.367f, 0.400f, 880, 0, 0},
       {CueKind::Sound, 0.433f, 0.467f, 4445, 0, 0},
       {CueKind::Ffx, 0.500f, 0.667f, 268, 10120, 2}}},
     4},
    // Rakuyo, A->B (a000_19580; no weapon clip: the parts change at the switch)
    {270, true, 19580, 1.233f, 3, 0.267f, 0.267f, 0.267f, false, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.500f, 0.533f, 794, 0, 0},
       {CueKind::Ffx, 0.533f, 0.633f, 7114, 12120, -1}}},
     2},
    // Rakuyo, B->A (a000_19590; no weapon clip: the parts change at the switch)
    {270, false, 19590, 1.233f, 1, 0.433f, 0.420f, 0.426f, false, Support::Tested,
     nullptr,
     {{{CueKind::Ffx, 0.433f, 0.533f, 7114, 10120, -1},
       {CueKind::Sound, 0.467f, 0.500f, 794, 0, 0}}},
     2},
    // Boom Hammer, A->B (a000_19600)
    {280, true, 19600, 1.600f, 5, 0.433f, 0.430f, 0.430f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.400f, 0.467f, 602, 0, 0},
       {CueKind::Sound, 0.467f, 0.567f, 874, 0, 0},
       {CueKind::Ffx, 0.533f, 0.633f, 7131, 12130, -1}}},
     3},
    // Boom Hammer, B->A (a000_19610; no weapon clip: the parts change at the switch)
    {280, false, 19610, 0.000f, -1, -1.000f, 0.000f, 0.000f, false, Support::Vanilla,
     "no clip in this direction",
     {},
     0},
    // Bloodletter, A->B (a000_19620)
    {290, true, 19620, 3.167f, 3, 0.733f, 0.000f, 0.000f, true, Support::Vanilla,
     "the clip adds SpEffects 6160 / 6170 (the self-damage and the buff)",
     {{{CueKind::Sound, 0.600f, 0.633f, 610, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 4200, 0, 0},
       {CueKind::Sound, 0.667f, 0.767f, 900, 0, 0},
       {CueKind::Sound, 0.667f, 0.767f, 929, 0, 0},
       {CueKind::Ffx, 0.833f, 2.833f, 7121, 12130, 0},
       {CueKind::Ffx, 2.000f, 3.000f, 7122, 220, 1}}},
     6},
    // Bloodletter, B->A (a000_19630)
    {290, false, 19630, 1.600f, 1, 0.800f, 0.000f, 0.000f, true, Support::Vanilla,
     "the clip adds SpEffects 6160 / 6170 (the self-damage and the buff)",
     {{{CueKind::Sound, 0.500f, 0.533f, 610, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 4200, 0, 0},
       {CueKind::Ffx, 0.600f, 1.100f, 7120, 12130, 0}}},
     3},
    // Church Pick, A->B (a000_19640)
    {300, true, 19640, 1.233f, 3, 0.600f, 0.460f, 0.460f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.400f, 0.500f, 740, 0, 0},
       {CueKind::Ffx, 0.567f, 0.667f, 7100, 10130, -1}}},
     2},
    // Church Pick, B->A (a000_19650)
    {300, false, 19650, 1.400f, 1, 0.567f, 0.000f, 0.000f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.367f, 0.400f, 741, 0, 0},
       {CueKind::Sound, 0.400f, 0.500f, 740, 0, 0},
       {CueKind::Ffx, 0.600f, 0.700f, 7100, 10130, -1}}},
     3},
    // Whirligig Saw, A->B (a000_19660)
    {310, true, 19660, 1.733f, 3, 0.467f, 0.467f, 0.467f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.400f, 0.500f, 935, 0, 0},
       {CueKind::Ffx, 0.500f, 1.233f, 7125, 12130, 0}}},
     2},
    // Whirligig Saw, B->A (a000_19670)
    {310, false, 19670, 2.200f, 1, 0.567f, 0.550f, 0.558f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.533f, 0.633f, 935, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 741, 0, 0},
       {CueKind::Ffx, 0.567f, 0.667f, 7100, 10130, -1},
       {CueKind::Sound, 1.233f, 1.267f, 8100, 0, 0}}},
     4},
    // Simon's Bowblade, A->B (a000_19680)
    {320, true, 19680, 1.500f, 3, 0.167f, 0.167f, 0.167f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.067f, 0.167f, 750, 0, 0},
       {CueKind::Sound, 0.500f, 0.533f, 910, 0, 0},
       {CueKind::Sound, 0.667f, 0.767f, 620, 0, 0},
       {CueKind::Ffx, 0.733f, 0.833f, 7100, 13110, -1}}},
     4},
    // Simon's Bowblade, B->A (a000_19690)
    {320, false, 19690, 1.400f, 1, 0.333f, 0.333f, 0.333f, true, Support::Tested,
     nullptr,
     {{{CueKind::Sound, 0.067f, 0.167f, 750, 0, 0},
       {CueKind::Sound, 0.567f, 0.667f, 620, 0, 0},
       {CueKind::Ffx, 0.667f, 0.767f, 7100, 10110, -1},
       {CueKind::Sound, 0.667f, 0.767f, 741, 0, 0}}},
     4},
    // weapon 370, A->B (a000_19700)
    {370, true, 19700, 0.000f, 3, 0.767f, 0.000f, 0.000f, true, Support::Vanilla,
     "no clip length",
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.467f, 1.867f, 7103, 3, 0},
       {CueKind::Sound, 0.633f, 0.733f, 944, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 945, 0, 0}}},
     4},
    // weapon 370, B->A (a000_19710)
    {370, false, 19710, 0.000f, 1, 0.567f, 0.000f, 0.000f, true, Support::Vanilla,
     "no clip length",
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.367f, 1.400f, 7103, 3, 0},
       {CueKind::Sound, 0.467f, 0.567f, 941, 0, 0},
       {CueKind::Sound, 0.533f, 0.600f, 931, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 945, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 940, 0, 0}}},
     6},
    // Kos Parasite, A->B (a000_19130; no weapon clip: the parts change at the switch)
    {380, true, 19130, 0.833f, 3, 0.200f, 0.000f, 0.000f, false, Support::Vanilla,
     "Kos Parasite: not a trick weapon",
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0}}},
     1},
    // Kos Parasite, B->A (a000_19280; no weapon clip: the parts change at the switch)
    {380, false, 19280, 0.833f, 1, 0.267f, 0.000f, 0.000f, false, Support::Vanilla,
     "Kos Parasite: not a trick weapon",
     {{{CueKind::Sound, 0.033f, 0.067f, 8100, 0, 0}}},
     1},
    // weapon 390, A->B (a000_19700)
    {390, true, 19700, 0.000f, 3, 0.767f, 0.000f, 0.000f, true, Support::Vanilla,
     "no clip length",
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.467f, 1.867f, 7103, 3, 0},
       {CueKind::Sound, 0.633f, 0.733f, 944, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 945, 0, 0}}},
     4},
    // weapon 390, B->A (a000_19710)
    {390, false, 19710, 0.000f, 1, 0.567f, 0.000f, 0.000f, true, Support::Vanilla,
     "no clip length",
     {{{CueKind::Sound, 0.033f, 0.133f, 750, 0, 0},
       {CueKind::Ffx, 0.367f, 1.400f, 7103, 3, 0},
       {CueKind::Sound, 0.467f, 0.567f, 941, 0, 0},
       {CueKind::Sound, 0.533f, 0.600f, 931, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 945, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 940, 0, 0}}},
     6},
    // weapon 400, A->B (a000_19720)
    {400, true, 19720, 1.833f, 3, 0.600f, 0.000f, 0.000f, true, Support::Vanilla,
     "the B->A clip fires bullets (event 2)",
     {{{CueKind::Sound, 0.000f, 0.100f, 951, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 953, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 952, 0, 0},
       {CueKind::Ffx, 0.633f, 1.633f, 7129, 53, 0},
       {CueKind::Ffx, 0.633f, 1.633f, 7127, 3, 1}}},
     5},
    // weapon 400, B->A (a000_19730; no weapon clip: the parts change at the switch)
    {400, false, 19730, 1.700f, 1, 0.467f, 0.000f, 0.000f, false, Support::Vanilla,
     "the B->A clip fires bullets (event 2)",
     {{{CueKind::Sound, 0.000f, 0.100f, 951, 0, 0},
       {CueKind::Sound, 0.300f, 0.400f, 952, 0, 0},
       {CueKind::Sound, 0.300f, 0.400f, 953, 0, 0},
       {CueKind::Ffx, 0.467f, 0.967f, 7130, 53, 0},
       {CueKind::Ffx, 0.467f, 0.967f, 7128, 3, 1}}},
     5},
    // weapon 410, A->B (a000_19720)
    {410, true, 19720, 1.833f, 3, 0.600f, 0.000f, 0.000f, true, Support::Vanilla,
     "the B->A clip fires bullets (event 2)",
     {{{CueKind::Sound, 0.000f, 0.100f, 951, 0, 0},
       {CueKind::Sound, 0.533f, 0.633f, 953, 0, 0},
       {CueKind::Sound, 0.633f, 0.733f, 952, 0, 0},
       {CueKind::Ffx, 0.633f, 1.633f, 7129, 53, 0},
       {CueKind::Ffx, 0.633f, 1.633f, 7127, 3, 1}}},
     5},
    // weapon 410, B->A (a000_19730; no weapon clip: the parts change at the switch)
    {410, false, 19730, 1.700f, 1, 0.467f, 0.000f, 0.000f, false, Support::Vanilla,
     "the B->A clip fires bullets (event 2)",
     {{{CueKind::Sound, 0.000f, 0.100f, 951, 0, 0},
       {CueKind::Sound, 0.300f, 0.400f, 952, 0, 0},
       {CueKind::Sound, 0.300f, 0.400f, 953, 0, 0},
       {CueKind::Ffx, 0.467f, 0.967f, 7130, 53, 0},
       {CueKind::Ffx, 0.467f, 0.967f, 7128, 3, 1}}},
     5},
}};
// clang-format on

} // namespace VR::Transform
