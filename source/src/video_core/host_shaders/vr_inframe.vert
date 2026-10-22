// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#version 450

// In-frame items (video_core/renderer_vulkan/vr_inframe.cpp): drawn into the game's scene colour
// with the game's own camera. Positions are in the camera record's view space (game units; x
// right, y up, z forward along the view); the projection is the game's D3D-style one (not
// reversed, depth 0..1), with NDC y flipped for Vulkan. The layout mirrors VrInFrame's
// DrawConstants (128 bytes).

layout(constant_id = 0) const float clip_scale_x = 1.0;
layout(constant_id = 1) const float clip_scale_y = 1.0;
layout(constant_id = 2) const float clip_offset_x = 0.0;
layout(constant_id = 3) const float clip_offset_y = 0.0;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_aux;

layout(push_constant, std430) uniform DrawData {
    vec4 mv[3];   // view <- mesh rows [R | t]
    vec4 color;   // linear rgb, a
    vec4 emissive; // Shade::Model: w = the grip's centre (mesh y)
    vec4 uv;      // u0 v0 u1 v1
    vec4 proj;    // P00, P11, A, B: clip = (P00 x, -P11 y, A z + B, z)
    vec4 extra;   // x shade, y exposure
} dd;

layout(location = 0) out vec3 out_view;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) out vec3 out_local; // mesh space (the cards' analytic edges)
// A game model: where the hand that holds it is on screen (frame uv; -1 = none): the mesh point
// (0, emissive.w, 0), the grip's centre, projected. The fragment reads the scene's light there.
layout(location = 4) flat out vec2 out_anchor;

void main() {
    const vec4 p = vec4(in_position, 1.0);
    const vec3 view0 = vec3(dot(dd.mv[0], p), dot(dd.mv[1], p), dot(dd.mv[2], p));
    // R's columns are the rows given; the normal goes by inverse(transpose(R^T)) = inverse(R).
    const mat3 r = mat3(dd.mv[0].xyz, dd.mv[1].xyz, dd.mv[2].xyz);
    const int shade = int(dd.extra.x + 0.5) & 15;
    // VialDrain (8), Syringe (10), NativeLiquid (14): chamber data replaces texture bounds.
    out_uv = (shade == 8 || shade == 10 || shade == 14) ? in_aux : mix(dd.uv.xy, dd.uv.zw, in_aux);
    out_local = in_position;
    vec3 view = view0;
    vec3 normal = inverse(r) * in_normal;
    // Shade::Steel (9) as a wire (uv.x > 0: the vial's needle, core/vr/vial_needle.h): the mesh's
    // vertices are on its axis (in_aux = (side -1 / +1, the radius there as a share of the
    // shaft's 0.5)); each goes out sideways, square to the axis and the eye, to the needle's edge
    // plus one pixel. out_local = (the distance from the axis in pixels, the half width drawn in
    // pixels: at least uv.x, the share of it that is needle); out_normal = the sideways direction
    // (the fragment rebuilds the round normal from both). uv.y = 1 / the frame's height.
    if (shade == 9 && dd.uv.x > 0.0 && dd.uv.y > 0.0 && view0.z > 1e-4) {
        // (v * r = R v: the mesh direction v in view.)
        const vec3 axis = normalize(vec3(0.0, 1.0, 0.0) * r);
        const vec3 to_eye = normalize(-view0);
        vec3 side = cross(axis, to_eye);
        if (dot(side, side) < 1e-10) {
            side = cross(axis, vec3(0.0, 1.0, 0.0)); // pointing at the eye: any side
        }
        side = normalize(side);
        const float pixel = 2.0 * view0.z * dd.uv.y / max(dd.proj.y * clip_scale_y, 1e-6);
        const float radius_px = length(vec3(0.5, 0.0, 0.0) * r) * in_aux.y / pixel;
        const float drawn_px = max(radius_px, dd.uv.x);
        const float half_px = drawn_px + 1.0;
        view = view0 + side * (in_aux.x * half_px * pixel);
        normal = side;
        out_local = vec3(in_aux.x * half_px, drawn_px, radius_px / max(drawn_px, 1e-6));
    }
    out_normal = normal;
    out_view = view;
    // Item::depth_pull (extra.x's bits 6 and up, in 1e-4 game units): depth-tested as if that
    // much nearer, along the ray to the eye (the pixel stays where it is), never nearer than just
    // past the near plane (-B / A).
    vec3 pv = view;
    const float pull = float(int(dd.extra.x + 0.5) >> 6) * 1e-4;
    if (pull > 0.0 && view.z > 1e-4) {
        const float near_z = -dd.proj.w / max(dd.proj.z, 1e-6);
        const float z = max(view.z - pull, min(view.z, near_z * 1.02));
        pv = view * (z / view.z);
    }
    gl_Position = vec4(dd.proj.x * pv.x, -dd.proj.y * pv.y, dd.proj.z * pv.z + dd.proj.w, pv.z);
    const vec2 clip_scale = vec2(clip_scale_x, clip_scale_y);
    const vec2 clip_offset = vec2(clip_offset_x, clip_offset_y);
    gl_Position.xy = gl_Position.xy * clip_scale + gl_Position.w * clip_offset;
    out_anchor = vec2(-1.0);
    // Held models, including liquid bottles: light sampled at the holding hand.
    if (shade == 4 || shade == 8 || shade == 9 || shade == 10 || shade == 11 || shade == 14) {
        // (Steel: the fist is emissive.xyz in its own mesh.)
        const vec4 a = shade == 9 ? vec4(dd.emissive.xyz, 1.0) : vec4(0.0, dd.emissive.w, 0.0, 1.0);
        const vec3 av = vec3(dot(dd.mv[0], a), dot(dd.mv[1], a), dot(dd.mv[2], a));
        if (av.z > 1e-3) {
            out_anchor = (vec2(dd.proj.x * av.x, -dd.proj.y * av.y) / av.z *
                          clip_scale + clip_offset) * 0.5 + 0.5;
        }
    }
}
