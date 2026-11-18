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
    float riposte; // native VR: a visceral's burst (vr_riposte.glsl), 0 .. 1
    uint roar_palette;
    vec4 ritual;      // native VR: the Kos ritual's vision (vr_kos_ritual.glsl)
    vec4 ritual_view; // eye size in pixels
    vec4 ritual_fov; // tangent left, right, up, down
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

#ifdef VR_RIPOSTE
// The native eyes' burst variant (with VR_NATIVE_COLOR; vr.ini riposte_style, run only while the
// burst shows): the view's brightness for both eyes, this frame's (vr_riposte_meter.comp).
layout (binding = 1, std430) readonly buffer RiposteMeter {
    vec4 riposte_meter;
};
// The eye's layer, typed sRGB: linear on reading, filtered in linear light (the meter reads the
// same views).
vec3 RiposteFetch(vec2 p) {
    return texture(texSampler, p / vec2(textureSize(texSampler, 0))).rgb;
}
#include "vr_riposte.glsl"
#endif

#ifdef VR_NATIVE_COLOR
#ifdef VR_KOS_IRIS
layout(set = 0, binding = 2) uniform sampler2D kos_iris;
#endif
#include "vr_kos_ritual.glsl"
#ifdef VR_KOS_IRIS
float KosRitualPixelSpan() {
    const vec2 span = abs(pp.ritual_fov.yw - pp.ritual_fov.xz) / max(pp.ritual_view.xy, vec2(1.0));
    return max(span.x, span.y) * KosRitualScale;
}
#endif
// The ritual's fold: the eye's own pixels (typed sRGB: linear on reading) at a point of the view's
// frame, through the eye's fields into its crop of the layered frame.
vec3 KosRitualFetch(vec2 c) {
    const vec2 t = c / KosRitualScale;
    const vec2 f = (t - pp.ritual_fov.xz) / (pp.ritual_fov.yw - pp.ritual_fov.xz);
    const vec2 p = vec2(pp.source_offset) +
        clamp(f * pp.ritual_view.xy, vec2(0.5), pp.ritual_view.xy - vec2(0.5));
    return texture(texSampler, p / vec2(textureSize(texSampler, 0))).rgb;
}
#endif

void main() {
#ifdef VR_NATIVE_COLOR
    vec4 color_linear = texelFetch(texSampler, ivec2(gl_FragCoord.xy) + pp.source_offset, 0);
#else
    vec4 color_linear = texture(texSampler, uv);
#endif
#ifdef VR_RIPOSTE
    const float riposte_l4 = RiposteLuma(color_linear.rgb); // the texel, as the taps read theirs
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
#ifdef VR_RIPOSTE
    // A visceral's burst, over everything else (vr.ini riposte_style).
    color_linear.rgb = RiposteStyle(color_linear.rgb, riposte_l4,
                                    gl_FragCoord.xy + vec2(pp.source_offset),
                                    vec2(textureSize(texSampler, 0)), riposte_meter.x, pp.riposte,
                                    pp.roar_palette != 0u);
#endif
    // The Kos ritual's vision (kos_visions), at this pixel's place in the view's frame.
    if (pp.ritual.x > 0.0001 || pp.ritual.y > 0.0001) {
        const vec2 f = gl_FragCoord.xy / max(pp.ritual_view.xy, vec2(1.0));
        const vec2 c = mix(pp.ritual_fov.xz, pp.ritual_fov.yw, f) * KosRitualScale;
        color_linear.rgb = KosRitual(color_linear.rgb, c, pp.ritual);
    }
    color = color_linear;
#else
    if (pp.hdr) {
        color = color_linear;
    } else {
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
#endif
}
