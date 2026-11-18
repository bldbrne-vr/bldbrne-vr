// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

// The game's frame rate set by hooks in its code instead of the community's 60 FPS++ patch
// (ported from bbport, deadinside28/bloodborne_pc gpu/shim/bbport_frame_rate.cpp, the same 1.09
// executable): the game steps by each frame's measured time, and what it ties to a 30 FPS frame
// follows that time too, so its timing is right at any frame rate, through hitches and caps.
// On by default (vr.ini frame_hooks=0: the 60 FPS++ patch instead; VR_FRAME_HOOKS=1 / 0 forces
// it); game_fps the frame rate (0 = the headset's refresh). See bloodborne_frame_rate.cpp.
namespace VR::Bloodborne {

// Patch time (memory_patcher, before the game's first instruction): the hooks will be installed,
// so the community's frame rate patches are left out (the hooks need the game's own code).
[[nodiscard]] bool FrameHooksRequested();
// The eboot is loaded (after the patches): installs the hooks when requested.
void FrameRateEbootLoaded(VAddr base, u64 size);

// The strange moon's flag (core/vr/game_area.h BloodMoon), read now and then: once a tick of the
// body's render tick, on the game's loop thread; frame hooks or not.
void StrangeMoonTick();
// The hooks are in: the game's frame rate is ours (its flip rate request is ignored).
[[nodiscard]] bool FrameHooksActive();
// The highest frame rate the game runs at (its own pacer waits for it), live. Clamped to
// 20..MaxRate; the step stays at most 1/30 s, so below 30 the game slows down, as it always did.
// Publishes a request, consumed at the next game-frame step without blocking
// the presenter. Selected cutscenes temporarily limit it to 30.
// GameFrameRate returns the effective rate; ending/skipping restores the latest request.
void SetGameFrameRate(int fps);
[[nodiscard]] int GameFrameRate();
// The game's measured frame time, smoothed (tau 0.5 s), while the hooks are in.
[[nodiscard]] float GameStepSeconds();
// VR_FRAME_TRACE=<ms> (tests): logs the main player's position at most every <ms> (any thread).
void FrameTraceTick();

} // namespace VR::Bloodborne
