// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_samplerless_texture_functions : require

// VrDepthRescale's draw (vr_depth_rescale.h): the source depth at this pixel, by the nearest rule
// of vkCmdBlitImage (the texel whose area holds the target pixel's centre), written as depth.
layout(set = 0, binding = 0) uniform texture2DArray source;

layout(push_constant) uniform Push {
    uvec2 src_size;
    vec2 scale; // source texels per target pixel
    uint layer;
} push;

void main() {
    // gl_FragCoord is the pixel's centre: (x + 0.5) * scale, the blit's own rule, in float.
    const uvec2 texel = min(uvec2(gl_FragCoord.xy * push.scale), push.src_size - 1u);
    gl_FragDepth = texelFetch(source, ivec3(texel, push.layer), 0).r;
}
