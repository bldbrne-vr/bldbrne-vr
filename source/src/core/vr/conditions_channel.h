// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/vr/ui_conditions.h"

// The game's status build-up meter rows for the UI layer (vr.ini ui_conditions; the reader is
// core/vr/bloodborne_status.cpp, the piece VR::Ui::Conditions).
namespace VR::Conditions {

// Any thread: the rows the game's HUD filled last (its static row block, read now). {0, 0} when
// the block is unknown (a signature mismatch, not installed).
[[nodiscard]] VR::Ui::Conditions::Rows Latest();

} // namespace VR::Conditions
