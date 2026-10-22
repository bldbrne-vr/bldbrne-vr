// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// Live tuning for VR look-and-feel work (debug only): with VR_TUNE_FILE=<path> set, Tune(key,
// fallback) returns the value of a `key=value` line of that file, which is read again whenever it
// changes (checked at most every 0.25 s), so a test run can try placements, grips and colours
// without a rebuild. Without the variable every call returns `fallback` at once (one relaxed
// atomic load). Any thread.
namespace VR::Tune {

// True when VR_TUNE_FILE is set (the values may differ from the code's).
[[nodiscard]] bool Active();

[[nodiscard]] float Get(const char* key, float fallback);

} // namespace VR::Tune
