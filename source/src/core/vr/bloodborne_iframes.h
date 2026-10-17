// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) longer dodge invulnerability (vr.ini dodge_iframes_extra, default 2)
// and its end by an attack during the dodge (dodge_attack, dodge_attack_iframes=0; the hook is
// installed for either):
// after a quickstep's or roll's own i-frames (TAE event 8, 0-0.367 s; Old Hunter Bone 0-0.267 s)
// the hunter stays invulnerable for N more of the game's 30 fps frames (N/30 s, rounded to whole
// updates). Backsteps have no i-frames in the game and get none; jumps, knockdowns and every
// other animation stay vanilla; only the main player.
//
// How: TAE event 8 sets bit 1 of the action flags ([[PlayerIns+0x3B0]+0x08]+0x1BC) on every
// behaviour step inside its window, and PlayerIns::Update zeroes them at its end. A detour at the
// entry of PlayerIns vt+0xE8 (0x18F7FB0, after the behaviour step) sets the same bit for the extra
// updates (core/vr/iframe_extend.h), so an extension update is exactly a vanilla i-frame update:
// attacks flagged to ignore i-frames still hit, throws are unaffected, and there is no perfect
// invincibility. Every site is signature-checked; a mismatch installs nothing.
//
// Logs: VR_IFRAMES_LOG=1 one line per dodge (clip, vanilla window, extended end, game and wall
// time), backsteps, F8 windows not extended, a check at PlayerIns::Update (the game's own
// IsInvincible over each protected run) and STATS every 10 s; =2 adds a line per update around
// dodges. VR_IFRAMES_DRYRUN=1 computes and logs but never writes the bit (vanilla windows).
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before guest threads exist): checks every site and installs.
void InstallIframes(VAddr base, u64 size);

// Main player update (site A, inside PlayerIns::Update before its reset): the logging check of
// what the game sees (the bit, IsInvincible). Does nothing unless installed with a log level.
void IframesSiteA(u8* player);

[[nodiscard]] bool IsIframesInstalled();

// Attacks through dodges (vr.ini dodge_attack, core/vr/anim_classes.h GateOut::dodge_attack): a
// physical swing (`what` "swing") or a gun shot ("shot", `serial` its window or pull) started
// during a dodge. With dodge_attack_iframes=0 the dodge's invulnerability ends at the next
// main-player update (the F8 bit is cleared for the rest of that dodge and it gets no extension,
// core/vr/iframe_extend.h Input::cut); a request that lands before the dodge's i-frame window
// opens waits for it (pending until the dodge clip ends or the next one starts). With 1 nothing
// changes. Any thread (one atomic flag, taken by the hook's thread).
void IframesAttackCut(const char* what, u32 serial);

} // namespace VR::Bloodborne
