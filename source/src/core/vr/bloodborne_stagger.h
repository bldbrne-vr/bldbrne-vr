// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Bloodborne (v1.09 eboot) shorter hit staggers (vr.ini stagger_shorten_ms, default 150; 0 = the
// game's): the hunter's hit staggers (a000_020000 .. 020399) play faster from their start until
// they have gained stagger_shorten_ms, so the game frees him that much sooner (every window of the
// clip and its end); core/vr/stagger_speed.h has the rule.
//
// How: ChrIns::Update (0x18BD6A0) takes the character's frame time from its time module (the hit
// stop's slow-down applied) and stores it at ChrIns+0xE0 and in its own frame (rbp-0x7C), the
// time the rest of the hunter's update runs on (his behaviour graph, the TAE, the root motion). A
// detour at that store (0x18BD716, only the main player reaches the host) scales it while a
// stagger is sped up. Every site is signature-checked; a mismatch installs nothing.
//
// Logs: one line per sped-up stagger (`VR stagger: ...`); VR_STAGGER_LOG=1 adds STATS every 10 s.
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled; before guest threads exist): checks the site and installs.
void InstallStaggerSpeed(VAddr base, u64 size);

[[nodiscard]] bool IsStaggerSpeedInstalled();

} // namespace VR::Bloodborne
