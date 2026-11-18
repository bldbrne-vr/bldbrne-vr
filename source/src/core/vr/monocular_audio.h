// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include "core/vr/whip_audio.h"

// The Monocular's lens ticks (2026-10-30: "a small sound ... like a 'tac tac tac tac'
// like adjusting its lens ... make it feel more physical (without adding vibration)"): at the
// eye, one short tick for every DetentZoom the wrist turns the zoom through, so the trigger's
// coarse ring (three times the zoom per twist) ticks three times as fast; none past the limits
// (the lens's stop). The tick is synthesized here, not taken from the game. Mixed into the first
// main audio port with the other one-shots (Whip::RecallAudioMixer).
namespace VR::Monocular {
inline constexpr float DetentZoom = 0.1f;                // a tick every 0.1x
inline constexpr std::int64_t MinTickGapNs = 35'000'000; // at most about 28 a second
inline constexpr float TickGain = 0.25f;

// The presenter's input frame only (UpdateHeldRequest), every frame: `enabled` while the optic is
// at an eye, `tick` for a new one, `pan` toward that eye.
void PublishLensAudio(bool enabled, bool tick, float pan, std::int64_t now);
Whip::RecallAudioFrame LatestLensAudio();
// The tick: 24 kHz mono S16LE, made once.
std::span<const char> LensTickPcm();
} // namespace VR::Monocular
