// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cmath>

namespace VR::EyeHistory {
// DIBR's eye history when dibr_eye_taa is unset (-1): lighter than native's 0.85, the value the
// performance scan applies (Auto and a manual DIBR choice now get it too, 2026-10-21). Each DIBR
// eye is already a resampled crop of one centre frame, re-warped every refresh, so a heavy
// history smears it further. Never above the inherited weight (eye_taa=0 keeps it off).
inline constexpr float DibrDefault = 0.25f;
// Negative/non-finite values: unset (native inherits eye_taa / taa_eye_taa, DIBR DibrDefault).
inline float Override(float value) {
    return !std::isfinite(value) || value < 0.0f ? -1.0f : std::clamp(value, 0.0f, 0.9f);
}
inline float Weight(bool native_stereo, bool render_taa, float eye_taa,
                    float taa_eye_taa, float dibr_eye_taa) {
    const float inherited = render_taa && taa_eye_taa >= 0.0f ? taa_eye_taa : eye_taa;
    if (native_stereo)
        return inherited;
    return dibr_eye_taa >= 0.0f ? dibr_eye_taa : std::min(DibrDefault, inherited);
}
} // namespace VR::EyeHistory
