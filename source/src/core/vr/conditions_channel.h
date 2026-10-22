// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "core/vr/ui_conditions.h"

// The game's status build-up meter rows for the UI layer (vr.ini ui_conditions; the reader is
// core/vr/bloodborne_status.cpp, the piece VR::Ui::Conditions).
namespace VR::Conditions {

// Any thread: the rows the game's HUD filled last (its static row block, read now). {0, 0} when
// the block is unknown (a signature mismatch, not installed).
[[nodiscard]] VR::Ui::Conditions::Rows Latest();

} // namespace VR::Conditions
