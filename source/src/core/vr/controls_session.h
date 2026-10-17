// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "core/vr/controls_view.h"

// "Show VR controls" against the emulator's live state (vr_menu=1): the settings its words depend
// on, read when the view opens (the presenter's settings, the quick items', melee's, the body's).
namespace VR::ControlsView {

// The live settings (frame thread).
Config CurrentConfig();

// One line for the log: the layout and the controller the view was built for.
std::string Describe(const Config& c, Family family, std::string_view profile, bool dpad);

} // namespace VR::ControlsView
