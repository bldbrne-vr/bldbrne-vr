// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

layout(constant_id = 0) const bool VrColorEnabled = true;

vec4 VrColorDecode(uint packed) {
    if (packed == 0u) {
        return vec4(1.0, 1.0, 1.0, 0.0);
    }
    vec3 values = vec3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    vec3 factors = 1.0 + (values - 100.0) * 0.01;
    return vec4(factors.xy, 1.0 / factors.z, 0.0);
}

bool VrColorActive(vec4 settings) {
    return VrColorEnabled && any(notEqual(settings.xyz, vec3(1.0)));
}

vec3 VrColorGrade(vec3 color, vec4 settings) {
    color = clamp(color, 0.0, 1.0);
    if (settings.y != 1.0) {
        float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = clamp(mix(vec3(gray), color, settings.y), 0.0, 1.0);
    }
    if (settings.x != 1.0) {
        const float pivot = 0.18;
        bvec3 upper = greaterThan(color, vec3(pivot));
        vec3 span = mix(vec3(pivot), vec3(1.0 - pivot), upper);
        vec3 distance = mix(color, 1.0 - color, upper);
        vec3 curved = distance * span / (span + (settings.x - 1.0) * (span - distance));
        color = mix(curved, 1.0 - curved, upper);
    }
    if (settings.z != 1.0) {
        color = pow(max(color, vec3(0.0)), vec3(settings.z));
    }
    return clamp(color, 0.0, 1.0);
}
