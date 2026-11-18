// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_ARB_shader_stencil_export : require

// VrDepthRescale's draw (vr_depth_rescale.h), stencil: as vr_depth_copy.frag, the source's stencil
// value exported as this pixel's stencil reference (VK_EXT_shader_stencil_export; the pipeline
// replaces the stencil with it).
layout(set = 0, binding = 0) uniform utexture2DArray source;

layout(push_constant) uniform Push {
    uvec2 src_size;
    vec2 scale; // source texels per target pixel
    uint layer;
} push;

void main() {
    // gl_FragCoord is the pixel's centre: (x + 0.5) * scale, the blit's own rule, in float.
    const uvec2 texel = min(uvec2(gl_FragCoord.xy * push.scale), push.src_size - 1u);
    gl_FragStencilRefARB = int(texelFetch(source, ivec3(texel, push.layer), 0).r);
}
