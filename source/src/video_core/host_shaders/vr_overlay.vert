// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

// VR overlay meshes (video_core/renderer_vulkan/vr_overlay.cpp): world = OpenXR LOCAL space.
// The layouts mirror Vulkan::VrOverlay::FrameConstants / DrawConstants (vr_overlay_meshes.h).

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_aux;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_proj[2];
    vec4 eye_pos[2];
    vec4 key_dir;
    vec4 key_color;
    vec4 fill_dir;
    vec4 fill_color;
    vec4 sky; // w: exposure
    vec4 ground;
    vec4 rim_color;
    vec4 field; // what the headset shows: degrees left, right, up, down (0: unknown)
} fd;

layout(push_constant, std430) uniform DrawData {
    mat4 model;
    vec4 base;
    vec4 emissive;
    vec4 params;
    vec4 extra; // x eye, y shade mode, z fade, w aux fade
} dd;

layout(location = 0) out vec3 out_world;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_aux;

// The depth prepass and the EQUAL-tested blend pass must produce bit-identical depth.
invariant gl_Position;

void main() {
    const vec4 world = dd.model * vec4(in_position, 1.0);
    out_world = world.xyz;
    // Inverse transpose: correct for the laser's non-uniform scale too.
    out_normal = transpose(inverse(mat3(dd.model))) * in_normal;
    out_aux = in_aux;
    gl_Position = fd.view_proj[int(dd.extra.x + 0.5)] * world;
}
