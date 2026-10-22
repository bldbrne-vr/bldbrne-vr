// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
