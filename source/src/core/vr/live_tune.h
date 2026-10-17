// SPDX-License-Identifier: GPL-2.0-or-later

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
