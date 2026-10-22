// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#version 450
layout(binding=0) uniform sampler2D atlas;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 uv_rect;
    vec4 geometry;
    vec4 tint;
} pc;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    vec2 half_texel = 0.5 / vec2(textureSize(atlas, 0));
    vec2 sample_uv = clamp(uv, pc.uv_rect.xy + half_texel,
                          pc.uv_rect.xy + pc.uv_rect.zw - half_texel);
    color = texture(atlas, sample_uv, -0.5) * pc.tint;
}
