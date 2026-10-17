// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) cinematic detection for the flat mode (VR::UpdateCinematic,
// core/vr/cinematic_state.h). Once per main-player update it reads the state the last behaviour
// step left behind and steps the state machine. It only reads memory: the one code patch is the
// player update hook it shares with the instant injection (bloodborne_heal.cpp, site A).
//
// Signals, from the module bag = [PlayerIns+0x3B0]:
//  - throw: ThrowNode state [[[bag+0x88]+0x28]+0x20] (1/3/5 the hunter throws: viscerals,
//    ripostes, backstabs; 2/4/6 the hunter is thrown) and its alignment flag +0x64;
//  - event animation: bit 0x02 of [[bag+0x08]+0x10], set by HKS act 8 on every update of an
//    Event state (doors, levers, chests, lamps, scripted events), read only while the behaviour
//    module is ready ([[bag+0x30]+0x384]), like the engine's own IsEventAnim;
//  - animation ids, to name the category: the pending TimeAct entries ([bag+0x18]) and the last
//    requested event animation ([[PlayerIns+0x20]+4]);
//  - HP ([[bag+0x20]+0xF8]) for the death hold after a fatal grab (and ladder damage / death);
//  - ladders: the HKS ladder action state [[[PlayerIns+0x58]+0x38]+0x60] (-1 off the ladder),
//    the ladder object [[bag+0x58]+0x28] (rung counter, for the climb left) and the ladder
//    animations among the pending TimeAct entries (Cinematic::LadderGate decides).
// The engine code behind each signal is signature-checked at install, and the module vtables
// and owners on the first update and on every rising edge; a failed check turns that signal off
// for the session and VR behaves as without the feature.
namespace VR::Bloodborne {

// Install time (VR enabled, vr.ini cinematic=1): checks the engine code behind each signal and
// logs which signals are on. `site_ok` / `main_ok`: the player update hook's own checks.
// Returns true when the hook should run the detection (at least one signal is on).
bool InitCinematic(VAddr base, u64 size, bool site_ok, bool main_ok);

// The main player's update (hook site A, r13 = PlayerIns*; usually a job worker thread, one
// update at a time).
void CinematicUpdate(u8* player);

// The main player's pose copy (core/vr/bloodborne_body.cpp, after the frame's TAE step): the
// game's cancel window, the requests the playing clip accepts (the action request module, its
// owner checked by the caller). At site A it still reads empty during throws, so the cinematic's
// control back uses this copy (the latest one, at most 150 ms old; without the VR body's pose
// hook there is none and animations return at their end).
void NoteCancelWindow(const u8* player, u64 allow, bool move);

} // namespace VR::Bloodborne
