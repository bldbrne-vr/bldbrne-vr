// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    float health;
    float desaturation;
    vec4 color_adjust;
    ivec2 source_offset;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

#include "vr_color.glsl"

void main() {
#ifdef VR_NATIVE_COLOR
    vec4 color_linear = texelFetch(texSampler, ivec2(gl_FragCoord.xy) + pp.source_offset, 0);
#else
    vec4 color_linear = texture(texSampler, uv);
#endif
    if (VrColorActive(pp.color_adjust)) {
        color_linear.rgb = VrColorGrade(color_linear.rgb, pp.color_adjust);
    }
    // Native VR combat feedback uses typed sRGB input/output: work in linear light
    // (core/vr/vital_color.h): out of stamina the view drains of colour and dims, low HP brings a
    // dark red in from the edges.
    if (pp.health > 0.0001 || pp.desaturation > 0.0001) {
        const float desaturation = clamp(pp.desaturation, 0.0, 0.75);
        float gray = dot(color_linear.rgb, vec3(0.2126, 0.7152, 0.0722));
        color_linear.rgb = mix(color_linear.rgb, vec3(gray), desaturation);
        color_linear.rgb *= 1.0 - 0.3 * desaturation;
        float edge = smoothstep(0.30, 1.00, length(uv * 2.0 - 1.0));
        color_linear.rgb = mix(color_linear.rgb, vec3(0.16, 0.008, 0.012),
                               clamp(pp.health, 0.0, 0.45) * edge * edge);
    }
#ifdef VR_NATIVE_COLOR
    color = color_linear;
#else
    if (pp.hdr) {
        color = color_linear;
    } else {
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
#endif
}
