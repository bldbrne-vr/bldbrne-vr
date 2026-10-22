// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <cmath>
#include <cstdlib>
#include <optional>

namespace VR::EyeDistance {
inline constexpr float Default = 0.064f;

inline float Headset(float metres) {
    return metres >= 0.04f && metres <= 0.09f ? metres : Default;
}

// Capture once in CameraRecord: renderer workers must not read live settings or a newer head
// sample while building the two eyes, their lights, and their screen effects.
inline float Effective(float headset, float manual_mm, float scale = 1.0f) {
    if (manual_mm >= 40.0f && manual_mm <= 120.0f)
        return manual_mm * 0.001f;
    const float value = Headset(headset) * scale;
    return std::isfinite(value) && value > 0.0f ? value : Default;
}

// Explicit numeric VR_NATIVE_IPD overrides remain available for experiments, including 0
// (mono). Unset, "auto", and malformed values use the frame's settings/headset distance.
inline std::optional<float> NativeOverride(const char* text) {
    if (!text)
        return std::nullopt;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0.0f)
        return std::nullopt;
    return value;
}

inline float Native(float frame_ipd, std::optional<float> override) {
    if (override)
        return *override;
    return std::isfinite(frame_ipd) && frame_ipd > 0.0f ? frame_ipd : Default;
}
} // namespace VR::EyeDistance
