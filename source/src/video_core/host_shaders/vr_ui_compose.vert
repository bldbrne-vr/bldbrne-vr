// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#version 450
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 uv_rect;
    vec4 geometry; // width, height, radius, central angle
    vec4 tint;
} pc;
layout(location=0) out vec2 uv;
void main() {
    const vec2 corner[6] = vec2[6](vec2(0,0), vec2(0,1), vec2(1,0),
                                 vec2(1,0), vec2(0,1), vec2(1,1));
    int columns = pc.geometry.z > 0.0 ? 32 : 1;
    vec2 t = corner[gl_VertexIndex % 6];
    t.x = (float(gl_VertexIndex / 6) + t.x) / float(columns);
    float a = (t.x - 0.5) * pc.geometry.w;
    vec3 p = vec3((t.x - 0.5) * pc.geometry.x, (0.5 - t.y) * pc.geometry.y, 0);
    if (pc.geometry.z > 0.0) p.xz = pc.geometry.z * vec2(sin(a), -cos(a));
    gl_Position = pc.mvp * vec4(p, 1);
    uv = pc.uv_rect.xy + t * pc.uv_rect.zw;
}
